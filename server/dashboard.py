"""Sticky dashboard; layout stays independent of wire encoding."""

import io
import time
import math
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

import weather

from metrics import Metric
from russian import plural_form
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 800, 480
NEXT_UPDATE_SECONDS = 3600
SHOPPING_STALE_AFTER_SECONDS = 45 * 60
# These rectangles belong to firmware indicators, including their backgrounds.
LOCAL_REGIONS = ((176, 8, 223, 39),)


@lru_cache(maxsize=1)
def indicator_font() -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(Path(__file__).with_name("fonts") / "FreeSansBold.ttf"), 24)


def draw_indicators(draw: ImageDraw.ImageDraw, battery_pct: int | None,
                    temperature_c: float | None, humidity_pct: float | None) -> None:
    """The device's former top-margin indicators, now part of the server frame."""
    draw.rectangle((16, 12, 57, 35), fill=0)
    draw.rectangle((18, 14, 55, 33), fill=1)
    draw.rectangle((58, 19, 61, 28), fill=0)
    if battery_pct is not None and battery_pct > 0:
        width = (34 * battery_pct + 99) // 100
        draw.rectangle((20, 16, 19 + width, 31), fill=0)
    font = indicator_font()
    label = "?" if battery_pct is None else f"{battery_pct}%"
    draw.text((72, 32), label, font=font, fill=0, anchor="ls")
    temperature = "--" if temperature_c is None else f"{temperature_c:.1f}"
    humidity = "--" if humidity_pct is None else f"{humidity_pct:.1f}"
    draw.text((WIDTH - 16, 32), f"{temperature}°  {humidity}%",
              font=font, fill=0, anchor="rs")


def render(
    battery_pct: int | None = None,
    temperature_c: float | None = None,
    humidity_pct: float | None = None,
    shopping: Metric | None = None,
    forecast: weather.Forecast | None = None,
) -> Image.Image:
    """One monochrome frame, in visible-screen coordinates (buttons on top)."""
    frame = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(frame)
    font_path = str(Path(__file__).with_name("fonts") / "FreeSansBold.ttf")
    title = ImageFont.truetype(font_path, 36)
    number = ImageFont.truetype(font_path, 112)
    draw_indicators(draw, battery_pct, temperature_c, humidity_pct)
    draw.line((16, 50, WIDTH - 16, 50), fill=0, width=2)
    draw.text((40, 102), "Список покупок", font=title, fill=0)
    draw.text((40, 160), "—" if shopping is None else str(shopping.value), font=number, fill=0)
    label = "пунктов" if shopping is None else plural_form(
        shopping.value, "пункт", "пункта", "пунктов"
    )
    draw.text((44, 296), label, font=indicator_font(), fill=0)
    if shopping is None:
        draw.text((44, 390), "Данные ещё не получены", font=ImageFont.truetype(font_path, 20), fill=0)
    elif time.time() - shopping.updated_at > SHOPPING_STALE_AFTER_SECONDS:
        draw.text((44, 390), "Данные устарели", font=ImageFont.truetype(font_path, 20), fill=0)
    draw_weather(draw, forecast)
    for region in LOCAL_REGIONS:
        draw.rectangle(region, fill=1)
    return frame


def png(frame: Image.Image) -> bytes:
    output = io.BytesIO()
    frame.save(output, format="PNG")
    return output.getvalue()


@lru_cache(maxsize=8)
def weather_font(size: int) -> ImageFont.FreeTypeFont:
    return ImageFont.truetype(str(Path(__file__).with_name("fonts") / "FreeSansBold.ttf"), size)


def draw_weather_icon(draw: ImageDraw.ImageDraw, x: int, y: int, code: int) -> None:
    """Small monochrome symbols, drawn without emoji or external assets."""
    if code in (0, 1, 2):
        draw.ellipse((x + 16, y + 10, x + 40, y + 34), outline=0, width=2)
        for angle in range(0, 360, 45):
            a = math.radians(angle)
            draw.line((x + 28 + 17 * math.cos(a), y + 22 + 17 * math.sin(a),
                       x + 28 + 23 * math.cos(a), y + 22 + 23 * math.sin(a)), fill=0, width=2)
        if code == 0:
            return
    draw.ellipse((x + 8, y + 23, x + 33, y + 45), fill=1, outline=0, width=2)
    draw.ellipse((x + 24, y + 15, x + 49, y + 45), fill=1, outline=0, width=2)
    draw.ellipse((x + 39, y + 26, x + 61, y + 45), fill=1, outline=0, width=2)
    draw.rectangle((x + 20, y + 32, x + 49, y + 43), fill=1)
    draw.line((x + 20, y + 45, x + 49, y + 45), fill=0, width=2)
    if code in (45, 48):
        for offset in (51, 57):
            draw.line((x + 10, y + offset, x + 58, y + offset), fill=0, width=2)
    elif code >= 51:
        for offset in (20, 35, 50):
            if code in (71, 73, 75, 77, 85, 86):
                draw.text((x + offset - 4, y + 45), "*", font=weather_font(18), fill=0)
            else:
                draw.line((x + offset, y + 50, x + offset - 4, y + 58), fill=0, width=2)
        if code >= 95:
            draw.line((x + 36, y + 38, x + 29, y + 49, x + 37, y + 49,
                       x + 30, y + 63), fill=0, width=3)


def draw_weather(draw: ImageDraw.ImageDraw, forecast: weather.Forecast | None) -> None:
    draw.line((400, 86, 400, 424), fill=0, width=1)
    today = datetime.now(ZoneInfo(weather.TIMEZONE)).date()
    days = {} if forecast is None else {day.date: day for day in forecast.days}
    for offset, label in enumerate(("Сегодня", "Завтра")):
        day_date = today + timedelta(days=offset)
        y = 92 + offset * 190
        draw.text((432, y), f"{label} · {day_date:%d.%m}", font=weather_font(26), fill=0)
        day = days.get(day_date.isoformat())
        if day is None:
            draw.text((432, y + 51), "Нет прогноза", font=weather_font(24), fill=0)
            continue
        draw_weather_icon(draw, 430, y + 36, day.code)
        draw.text((508, y + 40), f"{day.low:.0f}° … {day.high:.0f}°",
                  font=weather_font(36), fill=0)
        draw.text((508, y + 83), weather.description(day.code), font=weather_font(22), fill=0)
        chance = "—" if day.precipitation is None else f"{day.precipitation}%"
        draw.text((432, y + 115), f"Осадки: {chance}", font=weather_font(22), fill=0)
    if forecast is not None and time.time() - forecast.updated_at > weather.STALE_AFTER_SECONDS:
        draw.text((432, 444), "Прогноз устарел", font=weather_font(18), fill=0)
