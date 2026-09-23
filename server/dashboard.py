"""Sticky's diagnostic dashboard; layout stays independent of wire encoding."""

import io

from PIL import Image, ImageDraw, ImageFont

WIDTH, HEIGHT = 800, 480
NEXT_UPDATE_SECONDS = 3600
# These rectangles belong to firmware indicators, including their backgrounds.
LOCAL_REGIONS = ((176, 8, 223, 39), (240, 8, 271, 39))


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
    labels = (
        f"Battery: {battery_pct}%" if battery_pct is not None else "Battery: --",
        f"Temperature: {temperature_c:.1f} C" if temperature_c is not None else "Temperature: --",
        f"Humidity: {humidity_pct:.1f}%" if humidity_pct is not None else "Humidity: --",
    )
    for row, label in enumerate(labels):
        draw.text((32, 198 + row * 42), label, font=font, fill=0)
    draw.rectangle((32, 364, 127, 427), fill=0)
    draw.rectangle((144, 364, 239, 427), outline=0, width=2)
    draw.text((264, 378), "800 x 480 / 1 bit", font=font, fill=0)
    # Asymmetric marks make rotation and bit polarity easy to check on hardware.
    draw.rectangle((WIDTH - 33, 16, WIDTH - 17, 32), fill=0)
    draw.line((WIDTH - 65, HEIGHT - 33, WIDTH - 17, HEIGHT - 33), fill=0, width=3)
    for region in LOCAL_REGIONS:
        draw.rectangle(region, fill=1)
    return frame


def png(frame: Image.Image) -> bytes:
    output = io.BytesIO()
    frame.save(output, format="PNG")
    return output.getvalue()
