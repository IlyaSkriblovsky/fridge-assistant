"""Sticky dashboard; layout stays independent of wire encoding."""

import io
import time

from metrics import Metric
from russian import plural_form
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 800, 480
NEXT_UPDATE_SECONDS = 3600
SHOPPING_STALE_AFTER_SECONDS = 45 * 60
# These rectangles belong to firmware indicators, including their backgrounds.
LOCAL_REGIONS = ((176, 8, 223, 39), (240, 8, 271, 39))


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
) -> Image.Image:
    """One monochrome frame, in visible-screen coordinates (buttons on top)."""
    frame = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(frame)
    font_path = str(Path(__file__).with_name("fonts") / "FreeSansBold.ttf")
    title = ImageFont.truetype(font_path, 36)
    number = ImageFont.truetype(font_path, 112)
    draw_indicators(draw, battery_pct, temperature_c, humidity_pct)
    draw.line((32, 60, WIDTH - 32, 60), fill=0, width=2)
    draw.text((40, 102), "Список покупок", font=title, fill=0)
    draw.text((40, 160), "—" if shopping is None else str(shopping.value), font=number, fill=0)
    label = "пунктов" if shopping is None else plural_form(
        shopping.value, "пункт", "пункта", "пунктов"
    )
    draw.text((44, 296), label, font=indicator_font(), fill=0)
    if shopping is None:
        draw.text((44, 390), "Данные ещё не получены", font=indicator_font(), fill=0)
    elif time.time() - shopping.updated_at > SHOPPING_STALE_AFTER_SECONDS:
        draw.text((44, 390), "Данные давно не обновлялись", font=indicator_font(), fill=0)
    for region in LOCAL_REGIONS:
        draw.rectangle(region, fill=1)
    return frame


def png(frame: Image.Image) -> bytes:
    output = io.BytesIO()
    frame.save(output, format="PNG")
    return output.getvalue()
