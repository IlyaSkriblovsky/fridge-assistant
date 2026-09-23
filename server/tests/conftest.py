"""Offline fixtures: no credentials, network, or real service clients."""

import io
import socket
import wave
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import httpx
import pytest
import pytest_asyncio

import main
import metrics
import shopping_list


@pytest.fixture(autouse=True)
def offline(monkeypatch, tmp_path):
    monkeypatch.setenv("DATABASE_PATH", str(tmp_path / "dashboard.sqlite3"))
    metrics.initialize()
    monkeypatch.setattr(shopping_list, "refresh", lambda: None)
    for name in main.REQUIRED_ENV:
        monkeypatch.setenv(name, "test")

    def forbidden(*args, **kwargs):
        raise AssertionError("Unexpected external service or network access")

    monkeypatch.setattr(socket.socket, "connect", forbidden)
    monkeypatch.setattr(socket.socket, "connect_ex", forbidden)
    monkeypatch.setattr(socket, "getaddrinfo", forbidden)
    monkeypatch.setattr(main.genai, "Client", forbidden)
    monkeypatch.setattr(shopping_list.gkeepapi, "Keep", forbidden)
    monkeypatch.setattr(main, "lan_ip", lambda: "192.0.2.1")
    monkeypatch.setattr(main, "public_ip", lambda: "192.0.2.2")


@pytest.fixture
def gemini(monkeypatch):
    client = SimpleNamespace(aio=SimpleNamespace(
        models=SimpleNamespace(generate_content=AsyncMock()), aclose=AsyncMock()
    ))
    factory = Mock(return_value=client)
    monkeypatch.setattr(main.genai, "Client", factory)
    return client


@pytest_asyncio.fixture
async def client(gemini):
    async with main.lifespan(main.app):
        async with httpx.AsyncClient(
            transport=httpx.ASGITransport(app=main.app), base_url="http://test",
            headers={"Authorization": "Bearer test"},
        ) as client:
            yield client


@pytest.fixture
def wav():
    buffer = io.BytesIO()
    with wave.open(buffer, "wb") as output:
        output.setnchannels(1)
        output.setsampwidth(2)
        output.setframerate(16000)
        output.writeframes(b"\x01\x00" * 160)
    return buffer.getvalue()
