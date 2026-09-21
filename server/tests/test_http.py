import json
from unittest.mock import AsyncMock

import httpx
import pytest
from google.genai import errors

import main

pytestmark = pytest.mark.asyncio
PATHS = ["/audio", "/audio/fault/500", "/audio/fault/empty", "/audio/fault/slow"]


@pytest.fixture
def answer(monkeypatch):
    mock = AsyncMock(return_value="Добавлено — молоко, γάλα.\nЕщё строка.")
    monkeypatch.setattr(main.assistant, "answer", mock)
    return mock


@pytest.fixture
def sleep(monkeypatch):
    mock = AsyncMock()
    monkeypatch.setattr(main.asyncio, "sleep", mock)
    return mock


@pytest.mark.parametrize("path", PATHS)
@pytest.mark.parametrize("authorization", [None, "Bearer wrong", "Basic test"])
async def test_authentication(client, answer, path, authorization):
    client.headers.pop("Authorization")
    headers = {} if authorization is None else {"Authorization": authorization}
    reply = await client.post(path, content=b"audio", headers=headers)
    assert reply.status_code == 401
    assert reply.headers["www-authenticate"] == "Bearer"
    answer.assert_not_awaited()


async def test_audio_response_and_reassembled_wav(client, answer, gemini, wav):
    streamed = bytearray(wav)
    streamed[4:8] = streamed[40:44] = b"\xff" * 4

    async def body():
        for start in range(0, len(streamed), 17):
            yield bytes(streamed[start:start + 17])

    reply = await client.post("/audio", content=body(), headers={
        "Content-Type": "audio/wav", "Authorization": "bEaReR test"
    })
    assert reply.status_code == 200
    assert reply.json() == {"response": answer.return_value}
    answer.assert_awaited_once()
    assert answer.call_args.args[:2] == (gemini, wav)


@pytest.mark.parametrize("size,expected_calls", [(4, 1), (5, 0)])
async def test_size_limit(client, answer, monkeypatch, size, expected_calls):
    monkeypatch.setattr(main, "MAX_AUDIO_BYTES", 4)
    reply = await client.post("/audio", content=b"x" * size)
    assert reply.status_code == 200
    assert answer.await_count == expected_calls
    assert reply.json() == {"response": (
        answer.return_value if expected_calls else "Слишком длинная запись."
    )}


@pytest.mark.parametrize("error,message", [
    (errors.APIError(429, {}), "Gemini ответил ошибкой 429, попробуй ещё раз."),
    (httpx.ConnectError("offline"), "Не получилось связаться с Gemini, попробуй ещё раз."),
])
async def test_gemini_errors(client, answer, error, message):
    answer.side_effect = error
    reply = await client.post("/audio", content=b"audio")
    assert reply.status_code == 200
    assert reply.json() == {"response": message}


@pytest.mark.parametrize("path,status,body", [
    ("/audio/fault/500", 500, {"detail": "deliberate failure"}),
    ("/audio/fault/empty", 200, {}),
    ("/audio/fault/slow", 200, {"response": "Received 5 bytes, eventually"}),
])
async def test_fault_responses(client, answer, sleep, path, status, body):
    reply = await client.post(path, content=b"audio")
    assert reply.status_code == status
    assert reply.json() == body
    answer.assert_not_awaited()
    if path.endswith("slow"):
        sleep.assert_awaited_once_with(main.FAULT_DELAY_SECONDS)
    else:
        sleep.assert_not_awaited()


async def stream_request(path, events, token="test"):
    """Drive ASGI directly so response timing is observable before buffering."""
    pending = list(events)
    sent = []

    async def receive():
        assert pending, "Application read past the supplied request events"
        return pending.pop(0)

    async def send(message):
        assert not pending, "Application responded before draining the upload"
        sent.append(message)

    await main.app({
        "type": "http", "asgi": {"version": "3.0"}, "http_version": "1.1",
        "method": "POST", "scheme": "http", "path": path, "raw_path": path.encode(),
        "query_string": b"", "root_path": "", "server": ("test", 80),
        "client": ("test", 1234),
        "headers": [(b"authorization", f"Bearer {token}".encode())],
    }, receive, send)
    return sent


@pytest.mark.parametrize("path,token", [(path, "wrong") for path in PATHS] + [
    (path, "test") for path in PATHS
])
async def test_drains_before_response(client, answer, sleep, monkeypatch, path, token):
    monkeypatch.setattr(main, "MAX_AUDIO_BYTES", 2)
    events = [
        {"type": "http.request", "body": b"abc", "more_body": True},
        {"type": "http.request", "body": b"def", "more_body": True},
        {"type": "http.request", "body": b"ghi", "more_body": False},
    ]
    await stream_request(path, events, token)
    answer.assert_not_awaited()


@pytest.mark.parametrize("token", ["test", "wrong"])
async def test_disconnect_never_executes(client, answer, token):
    sent = await stream_request("/audio", [
        {"type": "http.request", "body": b"partial", "more_body": True},
        {"type": "http.disconnect"},
    ], token)
    answer.assert_not_awaited()
    assert sent[0]["status"] == (200 if token == "test" else 401)
    if token == "test":
        assert json.loads(sent[1]["body"]) == {"response": ""}


@pytest.mark.parametrize("missing", main.REQUIRED_ENV)
async def test_missing_settings(monkeypatch, missing):
    monkeypatch.delenv(missing)
    with pytest.raises(RuntimeError, match=missing):
        async with main.lifespan(main.app):
            pytest.fail("Startup must fail without required settings")


async def test_lifespan_creates_and_closes_client(gemini):
    async with main.lifespan(main.app):
        main.genai.Client.assert_called_once_with()
        assert main.app.state.gemini is gemini
        gemini.aio.aclose.assert_not_awaited()
    gemini.aio.aclose.assert_awaited_once_with()
