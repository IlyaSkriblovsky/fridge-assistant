import base64
import io

import pytest
from PIL import Image

import dashboard


@pytest.mark.asyncio
async def test_frame_and_png_encode_the_same_pixels(client):
    query = "battery_pct=76&temperature_c=-3.4&humidity_pct=48.2"
    raw = await client.get(f"/sticky/dashboard?{query}")
    png = await client.get(f"/sticky/dashboard?format=png&{query}")
    assert raw.status_code == png.status_code == 200
    assert raw.headers["Content-Type"] == "application/octet-stream"
    assert raw.headers["Dashboard-Format"] == "mono1-v1"
    assert raw.headers["Content-Length"] == "48000"
    assert raw.headers["Next-Update-After"] == png.headers["Next-Update-After"] == "3600"
    assert raw.headers["Cache-Control"] == png.headers["Cache-Control"] == "no-store"
    assert "Content-Encoding" not in raw.headers
    assert png.headers["Content-Type"] == "image/png"
    image = Image.open(io.BytesIO(png.content))
    assert image.size == (800, 480)
    assert image.mode == "1"
    # Decode independently of the production encoder: black is MSB-first 1.
    for y in range(480):
        for x in range(800):
            black = bool(raw.content[y * 100 + x // 8] & (0x80 >> (x % 8)))
            assert black == (image.getpixel((x, y)) == 0)
    assert image.getpixel((40, 380)) == 0
    assert image.getpixel((180, 380)) == 255
    for region in dashboard.LOCAL_REGIONS:
        left, top, right, bottom = region
        assert image.crop((left, top, right + 1, bottom + 1)).getextrema() == (255, 255)


@pytest.mark.asyncio
async def test_missing_sensors_and_changed_snapshot(client):
    absent = await client.get("/sticky/dashboard")
    measured = await client.get("/sticky/dashboard?battery_pct=0&temperature_c=0&humidity_pct=0")
    assert absent.status_code == measured.status_code == 200
    assert len(absent.content) == len(measured.content) == 48000
    assert absent.content != measured.content


@pytest.mark.asyncio
@pytest.mark.parametrize("query", [
    "battery_pct=-1", "battery_pct=101", "battery_pct=1.5", "battery_pct=no",
    "temperature_c=nan", "temperature_c=inf", "temperature_c=-inf",
    "humidity_pct=-1", "humidity_pct=101", "humidity_pct=nan", "format=jpeg",
])
async def test_invalid_query(client, query):
    assert (await client.get(f"/sticky/dashboard?{query}")).status_code == 422


@pytest.mark.asyncio
@pytest.mark.parametrize("format", ["mono1", "png"])
async def test_dashboard_requires_device_token(client, format):
    for value in ("", "Bearer wrong"):
        response = await client.get(f"/sticky/dashboard?format={format}", headers={"Authorization": value})
        assert response.status_code == 401
        assert response.headers["WWW-Authenticate"].startswith("Basic" if format == "png" else "Bearer")


@pytest.mark.asyncio
async def test_browser_password_is_same_secret_and_only_for_png(client):
    auth = "Basic " + base64.b64encode(b"sticky:test").decode()
    response = await client.get("/sticky/dashboard?format=png", headers={"Authorization": auth})
    assert response.status_code == 200
    assert response.content.startswith(b"\x89PNG")
    for url in ("/sticky/dashboard", "/audio/fault/500"):
        response = await client.request("POST" if url.startswith("/audio") else "GET", url,
                                        headers={"Authorization": auth})
        assert response.status_code == 401
    for credentials in (b"sticky:wrong", b"wrong:test"):
        auth = "Basic " + base64.b64encode(credentials).decode()
        assert (await client.get("/sticky/dashboard?format=png", headers={"Authorization": auth})).status_code == 401
    assert (await client.get("/sticky/dashboard?format=png", headers={"Authorization": "Basic !!!"})).status_code == 401
