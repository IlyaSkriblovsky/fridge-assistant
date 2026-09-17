"""Prototype server for the Seeed Sticky AI assistant.

Single endpoint: the device POSTs a WAV recording, the server stores it on disk
under a timestamped name and reports back what it got. No processing yet.
"""

from __future__ import annotations

import asyncio
import contextlib
import os
import socket
import urllib.request
import wave
from collections.abc import AsyncIterator
from datetime import datetime
from pathlib import Path

from fastapi import FastAPI, Request
from fastapi.responses import JSONResponse
# The form parser yields starlette's UploadFile; fastapi's is a subclass of it,
# so an isinstance check against the fastapi one would miss every upload.
from starlette.datastructures import UploadFile

HOST = os.environ.get("HOST", "0.0.0.0")
PORT = int(os.environ.get("PORT", "8000"))
RECORDINGS_DIR = Path(os.environ.get("RECORDINGS_DIR", "recordings"))

# The device may stream a long recording; never buffer it whole in memory.
CHUNK_SIZE = 64 * 1024

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


def describe_wav(path: Path) -> str:
    """Read back the WAV header so the device's audio params are visible in the log."""
    try:
        with wave.open(str(path), "rb") as wav:
            frames = wav.getnframes()
            rate = wav.getframerate()
            duration = frames / rate if rate else 0.0
            return (
                f"{rate} Hz, {wav.getnchannels()} ch, "
                f"{wav.getsampwidth() * 8}-bit, {duration:.2f} s"
            )
    except (wave.Error, EOFError) as exc:
        return f"not a readable WAV ({exc})"


@contextlib.asynccontextmanager
async def lifespan(app: FastAPI) -> AsyncIterator[None]:
    RECORDINGS_DIR.mkdir(parents=True, exist_ok=True)
    lan = lan_ip()
    public = public_ip()
    print()
    print("  Assistant server")
    print(f"  recordings -> {RECORDINGS_DIR.resolve()}")
    print(f"  listening  -> {HOST}:{PORT}")
    print()
    print("  Put this URL into the device (same WiFi network):")
    print(f"    http://{lan or '<lan-ip-unknown>'}:{PORT}/audio")
    print(
        f"  Public IP (needs port forwarding to be reachable): {public or 'unavailable'}"
    )
    print()
    yield


app = FastAPI(title="Assistant Server", lifespan=lifespan)


@app.post("/audio")
async def upload_audio(request: Request) -> dict[str, object]:
    """Accept a WAV recording and store it under a timestamped name.

    Takes the audio either as the raw request body (any content type) or as a
    multipart form field — the device firmware may end up doing either.
    """
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")[:-3]
    path = RECORDINGS_DIR / f"{stamp}.wav"

    content_type = request.headers.get("content-type", "")
    size = 0
    with path.open("wb") as out:
        if content_type.startswith("multipart/form-data"):
            form = await request.form()
            for value in form.values():
                if not isinstance(value, UploadFile):
                    continue
                while chunk := await value.read(CHUNK_SIZE):
                    out.write(chunk)
                    size += len(chunk)
        else:
            async for chunk in request.stream():
                out.write(chunk)
                size += len(chunk)

    print(
        f"[{stamp}] {path.name}  {size} bytes  "
        f"content-type={content_type or '-'}  {describe_wav(path)}"
    )
    return {
        "status": "ok",
        "filename": path.name,
        "bytes": size,
        "response": f"Received {size} bytes",
    }



# --- Deliberate failures ------------------------------------------------------
#
# The firmware has four ways a round trip can end badly -- NO SERVER, SERVER
# ERROR, BAD RESPONSE, TIMED OUT -- and no way to provoke the last three against
# a backend that works. These three endpoints are exactly those cases, so a
# firmware test can walk the whole error table without anyone breaking the real
# endpoint to do it. They store nothing.


async def drain(request: Request) -> int:
    """Read the upload and throw it away.

    Every fault below answers without having a recording to store, and a
    response sent while the device is still uploading closes the connection
    under it -- which the firmware would report as NO SERVER instead of as the
    fault being tested.
    """
    size = 0
    async for chunk in request.stream():
        size += len(chunk)
    return size


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
