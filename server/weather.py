"""Open-Meteo daily forecasts, fetched independently of dashboard requests."""

import json
import math
import os
import time
from contextlib import closing
from dataclasses import asdict, dataclass
from datetime import date

import httpx

import metrics

REFRESH_SECONDS = 60 * 60
STALE_AFTER_SECONDS = 3 * REFRESH_SECONDS
TIMEZONE = "Asia/Nicosia"


@dataclass(frozen=True)
class Day:
    date: str
    low: float
    high: float
    precipitation: int | None
    code: int


@dataclass(frozen=True)
class Forecast:
    days: list[Day]
    updated_at: float


def coordinates() -> tuple[float, float] | None:
    lat, lon = os.getenv("WEATHER_LATITUDE"), os.getenv("WEATHER_LONGITUDE")
    if lat is None and lon is None:
        return None
    if lat is None or lon is None:
        raise ValueError("Set both WEATHER_LATITUDE and WEATHER_LONGITUDE")
    latitude, longitude = float(lat), float(lon)
    if not (-90 <= latitude <= 90 and -180 <= longitude <= 180):
        raise ValueError("Invalid weather coordinates")
    return latitude, longitude


def key() -> str:
    return json.dumps((coordinates(), TIMEZONE))


def parse(payload: dict) -> list[Day]:
    daily = payload["daily"]
    days = []
    for i, stamp in enumerate(daily["time"]):
        date.fromisoformat(stamp)
        low, high = daily["temperature_2m_min"][i], daily["temperature_2m_max"][i]
        chance = daily["precipitation_probability_max"][i]
        code = daily["weather_code"][i]
        if not all(isinstance(v, (int, float)) and math.isfinite(v) for v in (low, high)) or low > high:
            raise ValueError("Invalid weather temperatures")
        if chance is not None and (not isinstance(chance, int) or not 0 <= chance <= 100):
            raise ValueError("Invalid precipitation probability")
        if not isinstance(code, int):
            raise ValueError("Invalid weather code")
        days.append(Day(stamp, low, high, chance, code))
    if len(days) < 2 or len({day.date for day in days}) != len(days):
        raise ValueError("Incomplete daily forecast")
    return days


def refresh() -> None:
    location = coordinates()
    if location is None:
        return
    response = httpx.get("https://api.open-meteo.com/v1/forecast", params={
        "latitude": location[0], "longitude": location[1], "timezone": TIMEZONE,
        "forecast_days": 3,
        "daily": "temperature_2m_min,temperature_2m_max,precipitation_probability_max,weather_code",
    }, timeout=15)
    response.raise_for_status()
    days = parse(response.json())
    with closing(metrics.connect()) as db, db:
        db.execute("""INSERT INTO forecasts VALUES (?, ?, ?)
            ON CONFLICT(key) DO UPDATE SET payload=excluded.payload, updated_at=excluded.updated_at""",
            (key(), json.dumps([asdict(day) for day in days]), time.time()))


def snapshot() -> Forecast | None:
    if coordinates() is None:
        return None
    with closing(metrics.connect()) as db:
        row = db.execute("SELECT payload, updated_at FROM forecasts WHERE key=?", (key(),)).fetchone()
    return Forecast([Day(**day) for day in json.loads(row[0])], row[1]) if row else None


def description(code: int) -> str:
    if code == 0:
        return "Ясно"
    if code in (1, 2):
        return "Малооблачно"
    if code == 3:
        return "Облачно"
    if code in (45, 48):
        return "Туман"
    if code in (51, 53, 55, 56, 57):
        return "Морось"
    if code in (61, 63, 65, 66, 67, 80, 81, 82):
        return "Дождь"
    if code in (71, 73, 75, 77, 85, 86):
        return "Снег"
    if code in (95, 96, 99):
        return "Гроза"
    return "Нет описания"
