import base64
import io
import struct

import pytest
from PIL import Image

import dashboard
import main


@pytest.mark.asyncio
async def test_dashboard_returns_server_format_png(client):
    response = await client.get("/sticky/dashboard?battery_pct=76&temperature_c=-3.4&humidity_pct=48.2")
    assert response.status_code == 200
    assert response.headers["Content-Type"] == "image/png"
    assert response.headers["Dashboard-Format"] == "png"
    assert int(response.headers["Content-Length"]) == len(response.content)
    assert 0 < len(response.content) <= 65536
    assert response.headers["Next-Update-After"] == "3600"
    assert response.headers["Cache-Control"] == "no-store"
    assert "Content-Encoding" not in response.headers
    assert response.content[:8] == b"\x89PNG\r\n\x1a\n"
    assert struct.unpack(">IIBBBBB", response.content[16:29]) == (800, 480, 1, 0, 0, 0, 0)
    offset = 8
    chunks = []
    while offset < len(response.content):
        size = struct.unpack(">I", response.content[offset:offset + 4])[0]
        chunks.append(response.content[offset + 4:offset + 8])
        offset += size + 12
    assert offset == len(response.content)
    assert chunks[0] == b"IHDR" and chunks[-1] == b"IEND"
    assert chunks[1:-1] and set(chunks[1:-1]) == {b"IDAT"}
    image = Image.open(io.BytesIO(response.content))
    assert image.size == (800, 480)
    assert image.mode == "1"
    assert image.tobytes() == dashboard.render(76, -3.4, 48.2).tobytes()
    for left, top, right, bottom in dashboard.LOCAL_REGIONS:
        assert image.crop((left, top, right + 1, bottom + 1)).getextrema() == (255, 255)


@pytest.mark.asyncio
async def test_missing_sensors_and_changed_snapshot(client):
    absent = await client.get("/sticky/dashboard")
    measured = await client.get("/sticky/dashboard?battery_pct=0&temperature_c=0&humidity_pct=0")
    assert absent.status_code == measured.status_code == 200
    assert Image.open(io.BytesIO(absent.content)).mode == "1"
    assert Image.open(io.BytesIO(measured.content)).mode == "1"
    assert absent.content != measured.content


@pytest.mark.asyncio
@pytest.mark.parametrize("query", [
    "battery_pct=-1", "battery_pct=101", "battery_pct=1.5", "battery_pct=no",
    "temperature_c=nan", "temperature_c=inf", "temperature_c=-inf",
    "humidity_pct=-1", "humidity_pct=101", "humidity_pct=nan",
])
async def test_invalid_query(client, query):
    assert (await client.get(f"/sticky/dashboard?{query}")).status_code == 422


@pytest.mark.asyncio
async def test_format_is_not_an_endpoint_parameter(client):
    parameters = main.app.openapi()["paths"]["/sticky/dashboard"]["get"]["parameters"]
    assert {p["name"] for p in parameters} == {"battery_pct", "temperature_c", "humidity_pct"}
    expected = await client.get("/sticky/dashboard")
    # FastAPI ignores unknown query parameters; old URLs cannot select a codec.
    for value in ("mono1", "png", "jpeg"):
        response = await client.get(f"/sticky/dashboard?format={value}")
        assert response.status_code == 200
        assert response.content == expected.content
        assert response.headers["Content-Type"] == "image/png"


@pytest.mark.asyncio
async def test_dashboard_requires_device_token(client):
    for value in ("", "Bearer wrong"):
        response = await client.get("/sticky/dashboard", headers={"Authorization": value})
        assert response.status_code == 401
        assert response.headers["WWW-Authenticate"].startswith("Basic")


@pytest.mark.asyncio
async def test_browser_password_is_same_secret_and_only_for_dashboard(client):
    auth = "Basic " + base64.b64encode(b"sticky:test").decode()
    response = await client.get("/sticky/dashboard", headers={"Authorization": auth})
    assert response.status_code == 200
    assert response.content.startswith(b"\x89PNG")
    response = await client.post("/audio/fault/500", headers={"Authorization": auth})
    assert response.status_code == 401
    assert response.headers["WWW-Authenticate"] == "Bearer"
    for credentials in (b"sticky:wrong", b"wrong:test"):
        auth = "Basic " + base64.b64encode(credentials).decode()
        assert (await client.get("/sticky/dashboard", headers={"Authorization": auth})).status_code == 401
    assert (await client.get("/sticky/dashboard", headers={"Authorization": "Basic !!!"})).status_code == 401


@pytest.mark.parametrize("percent,filled", [(None, 0), (0, 0), (1, 1), (50, 17), (100, 34)])
def test_battery_fill_and_reserved_regions(percent, filled):
    frame = dashboard.render(percent, -40.0, 100.0)
    assert sum(frame.getpixel((x, 20)) == 0 for x in range(20, 54)) == filled
    assert frame.getpixel((16, 12)) == 0
    assert frame.getpixel((60, 24)) == 0
    for left, top, right, bottom in dashboard.LOCAL_REGIONS:
        assert frame.crop((left, top, right + 1, bottom + 1)).getextrema() == (1, 1)


def test_indicators_are_in_top_margin_and_sensors_are_independent():
    absent = dashboard.render()
    measured = dashboard.render(100, -40.0, 100.0)
    assert absent.crop((0, 40, 800, 480)).tobytes() == measured.crop((0, 40, 800, 480)).tobytes()
    assert measured.crop((72, 8, 160, 40)).getextrema() == (0, 1)
    assert measured.crop((560, 8, 784, 40)).getextrema() == (0, 1)
    for temperature, humidity in ((0, None), (None, 0)):
        partial = dashboard.render(temperature_c=temperature, humidity_pct=humidity)
        assert partial.crop((560, 8, 784, 40)).tobytes() != absent.crop((560, 8, 784, 40)).tobytes()
