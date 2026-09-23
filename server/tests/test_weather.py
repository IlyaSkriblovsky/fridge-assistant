from datetime import datetime, timedelta
from unittest.mock import Mock
from zoneinfo import ZoneInfo

import httpx
import pytest

import dashboard
import weather

refresh = weather.refresh


@pytest.fixture
def configured(monkeypatch):
    monkeypatch.setenv("WEATHER_LATITUDE", "35.1")
    monkeypatch.setenv("WEATHER_LONGITUDE", "33.3")


@pytest.fixture
def payload():
    today = datetime.now(ZoneInfo(weather.TIMEZONE)).date()
    return {"daily": {
        "time": [(today + timedelta(days=i)).isoformat() for i in range(3)],
        "temperature_2m_min": [18, 19, 20], "temperature_2m_max": [29, 30, 31],
        "precipitation_probability_max": [0, 70, None], "weather_code": [0, 61, 3],
    }}


def test_refresh_persists_and_failure_keeps_snapshot(configured, payload, monkeypatch):
    fetch = Mock(return_value=httpx.Response(200, json=payload,
        request=httpx.Request("GET", "https://api.open-meteo.com/v1/forecast")))
    monkeypatch.setattr(weather.httpx, "get", fetch)
    refresh()
    saved = weather.snapshot()
    assert saved.days[1].precipitation == 70
    assert fetch.call_args.kwargs["timeout"] == 15
    fetch.side_effect = httpx.ReadTimeout("offline")
    with pytest.raises(httpx.ReadTimeout):
        refresh()
    assert weather.snapshot() == saved
    monkeypatch.setenv("WEATHER_LATITUDE", "34.9")
    assert weather.snapshot() is None


@pytest.mark.parametrize("field,value", [
    ("temperature_2m_min", None), ("temperature_2m_max", float("nan")),
    ("precipitation_probability_max", 101), ("weather_code", None),
])
def test_invalid_payload_is_rejected(payload, field, value):
    payload["daily"][field][0] = value
    with pytest.raises(ValueError):
        weather.parse(payload)


def test_missing_coordinates_disable_weather():
    refresh()
    assert weather.snapshot() is None


@pytest.mark.parametrize("latitude,longitude", [("nan", "0"), ("91", "0"), ("0", "181")])
def test_invalid_coordinates(monkeypatch, latitude, longitude):
    monkeypatch.setenv("WEATHER_LATITUDE", latitude)
    monkeypatch.setenv("WEATHER_LONGITUDE", longitude)
    with pytest.raises(ValueError):
        weather.coordinates()


def test_render_selects_calendar_dates_and_preserves_statusbar(payload, monkeypatch):
    days = weather.parse(payload)
    forecast = weather.Forecast(days, 100)
    clock = Mock()
    clock.now.return_value = datetime.fromisoformat(days[1].date).replace(tzinfo=ZoneInfo(weather.TIMEZONE))
    monkeypatch.setattr(dashboard, "datetime", clock)
    texts = []
    original = dashboard.ImageDraw.ImageDraw.text
    def record(draw, xy, text, *args, **kwargs):
        texts.append(text)
        return original(draw, xy, text, *args, **kwargs)
    monkeypatch.setattr(dashboard.ImageDraw.ImageDraw, "text", record)
    frame = dashboard.render(76, 23.4, 48.2, forecast=forecast)
    assert "19° … 30°" in texts
    assert "18° … 29°" not in texts
    assert "Прогноз устарел" in texts
    assert frame.crop((0, 0, 800, 60)).tobytes() == dashboard.render(76, 23.4, 48.2).crop((0, 0, 800, 60)).tobytes()
    assert len(dashboard.png(frame)) < 65536


@pytest.mark.asyncio
async def test_dashboard_uses_weather_cache_only(client, configured, payload, monkeypatch):
    monkeypatch.setattr(weather.httpx, "get", Mock(return_value=httpx.Response(
        200, json=payload, request=httpx.Request("GET", "https://api.open-meteo.com/v1/forecast"))))
    refresh()
    monkeypatch.setattr(weather.httpx, "get", Mock(side_effect=AssertionError("Network during render")))
    response = await client.get("/sticky/dashboard")
    assert response.status_code == 200
    assert response.content == dashboard.png(dashboard.render(forecast=weather.snapshot()))
