"""Sticky's diagnostic dashboard; layout stays independent of wire encoding."""

import io
from functools import lru_cache
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 800, 480
NEXT_UPDATE_SECONDS = 3600
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
) -> Image.Image:
    """One monochrome frame, in visible-screen coordinates (buttons on top)."""
    frame = Image.new("1", (WIDTH, HEIGHT), 1)
    draw = ImageDraw.Draw(frame)
    font = ImageFont.load_default(size=24)
    title = ImageFont.load_default(size=44)
    draw.rectangle((0, 0, WIDTH - 1, HEIGHT - 1), outline=0)
    draw.text((32, 66), "STICKY / IDLE", font=title, fill=0)
    draw.text((32, 128), "Rendered on the server", font=font, fill=0)
    draw_indicators(draw, battery_pct, temperature_c, humidity_pct)
    draw.rectangle((32, 364, 127, 427), fill=0)
    draw.rectangle((144, 364, 239, 427), outline=0, width=2)
    draw.text((264, 378), "800 x 480 / 1 bit", font=font, fill=0)
    # Asymmetric marks make rotation and bit polarity easy to check on hardware.
    draw.line((WIDTH - 65, HEIGHT - 33, WIDTH - 17, HEIGHT - 33), fill=0, width=3)
    for region in LOCAL_REGIONS:
        draw.rectangle(region, fill=1)
    return frame


def png(frame: Image.Image) -> bytes:
    output = io.BytesIO()
    frame.save(output, format="PNG")
    return output.getvalue()
