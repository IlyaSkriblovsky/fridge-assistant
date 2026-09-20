"""Server for the Seeed Sticky voice assistant.

The device POSTs a question as a WAV recording to /audio, with its token in the
Authorization header. The recording is collected in memory, handed to the
assistant (Gemini and its tools, see assistant.py) and dropped; the assistant's
reply goes back as the text for the device's screen.
"""

from __future__ import annotations

import asyncio
import contextlib
import hmac
import io
import os
import socket
import time
import urllib.request
import wave
from collections.abc import AsyncIterator
from datetime import datetime

import httpx
from fastapi import Depends, FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse
from google import genai
from google.genai import errors
from starlette.requests import ClientDisconnect

import assistant

HOST = os.environ.get("HOST", "0.0.0.0")
PORT = int(os.environ.get("PORT", "8000"))

# Everything the server cannot work without; .env is where they live.
REQUIRED_ENV = (
    "GEMINI_API_KEY",
    "KEEP_EMAIL",
    "KEEP_MASTER_TOKEN",
    "KEEP_NOTE_ID",
    "DEVICE_TOKEN",
)

# The recording is held in memory, and this is as much of it as is kept. The
# device stops at 30 s, which is under 1 MB; Gemini takes up to 20 MB inline.
MAX_AUDIO_BYTES = 10 * 1024 * 1024

# What the device's WAV header says in both length fields. It streams the
# recording while the button is still held, so the header goes up before the
# recording has a length; the end of the chunked body is where it ends.
UNKNOWN_LENGTH = 0xFFFFFFFF

# How long /audio/fault/slow holds a question before answering. Longer than the
# device's config::kResponseTimeoutMs, which is 30 s, so the firmware gives up
# first and shows TIMED OUT.
FAULT_DELAY_SECONDS = 45


def lan_ip() -> str | None:
    """Address of this machine on the local network (what the device needs)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        # No packet is actually sent — this just picks the outbound interface.
        sock.connect(("8.8.8.8", 80))
        return sock.getsockname()[0]
    except OSError:
        return None
    finally:
        sock.close()


def public_ip(timeout: float = 2.0) -> str | None:
    try:
        with urllib.request.urlopen("https://api.ipify.org", timeout=timeout) as resp:
            return resp.read().decode().strip()
    except Exception:
        return None


def describe_wav(data: bytes) -> str:
    """Read back the WAV header so the device's audio params are visible in the log."""
    try:
        with wave.open(io.BytesIO(data), "rb") as wav:
            frames = wav.getnframes()
            rate = wav.getframerate()
            duration = frames / rate if rate else 0.0
            return (
                f"{rate} Hz, {wav.getnchannels()} ch, "
                f"{wav.getsampwidth() * 8}-bit, {duration:.2f} s"
            )
    except (wave.Error, EOFError) as exc:
        return f"not a readable WAV ({exc})"


def settle_wav_lengths(wav: bytearray) -> bool:
    """Write the true lengths into a WAV header that could not know them.

    Only fields that say UNKNOWN_LENGTH are touched, so a recording that arrived
    with real lengths is left as it came. Python's wave reads such a file
    anyway, as one of unknown length, but other readers and the log's duration
    do not. Returns True if anything was rewritten.
    """
    size = len(wav)
    if size < 12 or wav[:4] != b"RIFF" or wav[8:12] != b"WAVE":
        return False
    changed = False
    if int.from_bytes(wav[4:8], "little") == UNKNOWN_LENGTH:
        wav[4:8] = (size - 8).to_bytes(4, "little")
        changed = True

    # The data chunk is found by walking the chunks rather than assumed at
    # offset 36, which is where the device puts it but not where every WAV does.
    offset = 12
    while offset + 8 <= size:
        length = int.from_bytes(wav[offset + 4 : offset + 8], "little")
        if wav[offset : offset + 4] == b"data":
            if length == UNKNOWN_LENGTH:
                wav[offset + 4 : offset + 8] = (size - offset - 8).to_bytes(4, "little")
                changed = True
            break
        offset += 8 + length + (length & 1)
    return changed


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    missing = [name for name in REQUIRED_ENV if not os.environ.get(name)]
    if missing:
        raise RuntimeError(
            f"Set {', '.join(missing)}: run with --env-file .env (see docs/server.md)"
        )
    app.state.gemini = genai.Client()  # takes GEMINI_API_KEY from the environment

    lan = lan_ip()
    public = public_ip()
    print()
    print("  Assistant server")
    print(f"  model      -> {assistant.MODEL}")
    print(f"  Keep list  -> {os.environ['KEEP_NOTE_ID']}")
    print(f"  listening  -> {HOST}:{PORT}")
    print()
    print("  Put this URL into the device (same WiFi network):")
    print(f"    http://{lan or '<lan-ip-unknown>'}:{PORT}/audio")
    print(
        f"  Public IP (needs port forwarding to be reachable): {public or 'unavailable'}"
    )
    print()
    yield
    await app.state.gemini.aio.aclose()


async def drain(request: Request) -> int:
    """Read the upload and throw it away.

    Whatever answers without using the recording -- a refusal, and every fault
    below -- reads it to the end first: a response sent while the device is
    still uploading closes the connection under it, which the firmware would
    report as NO SERVER instead of as the answer it was sent.
    """
    size = 0
    async for chunk in request.stream():
        size += len(chunk)
    return size


async def authorize(request: Request) -> None:
    """Let through only the device: `Authorization: Bearer <DEVICE_TOKEN>`.

    Every endpoint depends on this. Anything else gets 401, which the device
    shows as SERVER ERROR 401.
    """
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    given = token.strip().encode()
    expected = os.environ["DEVICE_TOKEN"].encode()
    if scheme.lower() == "bearer" and hmac.compare_digest(given, expected):
        return

    with contextlib.suppress(ClientDisconnect):
        await drain(request)
    client = request.client.host if request.client else "-"
    reason = "wrong token" if header else "no token"
    print(f"[auth] {request.method} {request.url.path} from {client}: {reason}")
    raise HTTPException(401, headers={"WWW-Authenticate": "Bearer"})


app = FastAPI(
    title="Assistant Server", lifespan=lifespan, dependencies=[Depends(authorize)]
)


@app.post("/audio")
async def audio(request: Request) -> dict[str, str]:
    """Answer the question in a WAV recording with the text for the screen.

    The firmware streams the recording chunked while the button is held, so the
    body arrives over as long as the question took to ask. It is collected in
    memory, lengths filled in once it has all arrived, and dropped as soon as
    the assistant has it.
    """
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]

    def log(message: str) -> None:
        print(f"[{stamp}] {message}")

    content_type = request.headers.get("content-type", "")
    chunked = "chunked" in request.headers.get("transfer-encoding", "").lower()
    started = time.monotonic()
    wav = bytearray()
    size = 0
    try:
        async for chunk in request.stream():
            size += len(chunk)
            # Past the limit the rest is still read, only not kept: answering
            # before the body ends would cut the device off mid-upload, and it
            # would show NO SERVER instead of the answer below.
            if size <= MAX_AUDIO_BYTES:
                wav += chunk
    except ClientDisconnect:
        # The device has already shown an error and the question will be asked
        # again, so carrying this one out would do it twice.
        log(f"{size} bytes, then the device went away; nothing done")
        return {"response": ""}
    took = time.monotonic() - started
    # The device's wait for the answer starts here, at the end of the body.
    ended = time.monotonic()

    settled = settle_wav_lengths(wav)
    log(
        f"{size} bytes{' chunked' if chunked else ''} over {took:.2f} s  "
        f"content-type={content_type or '-'}  {describe_wav(wav)}"
        f"{'  (lengths filled in)' if settled else ''}"
    )

    if size > MAX_AUDIO_BYTES:
        log(f"over the {MAX_AUDIO_BYTES} byte limit, not sent to Gemini")
        reply = "Слишком длинная запись."
    else:
        try:
            reply = await assistant.answer(request.app.state.gemini, bytes(wav), log)
        except errors.APIError as exc:
            log(f"Gemini error: {exc}")
            reply = f"Gemini ответил ошибкой {exc.code}, попробуй ещё раз."
        except httpx.HTTPError as exc:
            log(f"Gemini unreachable: {exc!r}")
            reply = "Не получилось связаться с Gemini, попробуй ещё раз."
    log(f"reply: {reply}  ({time.monotonic() - ended:.2f} s after the body ended)")
    return {"response": reply}


# --- Deliberate failures ------------------------------------------------------
#
# The firmware has four ways a round trip can end badly -- NO SERVER, SERVER
# ERROR, BAD RESPONSE, TIMED OUT -- and no way to provoke the last three against
# a backend that works. These three endpoints are exactly those cases, so a
# firmware test can walk the whole error table without anyone breaking the real
# endpoint to do it. They store nothing.


@app.post("/audio/fault/500")
async def fault_500(request: Request) -> JSONResponse:
    """Answered, and not with 200. The device should show SERVER ERROR 500."""
    size = await drain(request)
    print(f"[fault] 500 after {size} bytes")
    return JSONResponse({"detail": "deliberate failure"}, status_code=500)


@app.post("/audio/fault/empty")
async def fault_empty(request: Request) -> dict[str, object]:
    """200 and valid JSON, with no `response` field. The device: BAD RESPONSE."""
    size = await drain(request)
    print(f"[fault] empty object after {size} bytes")
    return {}


@app.post("/audio/fault/slow")
async def fault_slow(request: Request) -> dict[str, object]:
    """Takes the question and says nothing. The device: TIMED OUT."""
    size = await drain(request)
    print(f"[fault] holding {size} bytes for {FAULT_DELAY_SECONDS} s")
    await asyncio.sleep(FAULT_DELAY_SECONDS)
    return {"response": f"Received {size} bytes, eventually"}


if __name__ == "__main__":
    import uvicorn

    uvicorn.run(app, host=HOST, port=PORT)
