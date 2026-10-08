"""Shared fresh dashboard rendering from server snapshots."""

from datetime import datetime, timezone

import dashboard
import device_readings
import shopping_list
import weather


def render(battery_pct=None, temperature_c=None, humidity_pct=None) -> bytes:
    return dashboard.png(dashboard.render(
        battery_pct, temperature_c, humidity_pct,
        shopping_list.snapshot(), weather.snapshot()))


def telegram_snapshot() -> tuple[bytes, str]:
    readings = device_readings.latest()
    values = {key: reading.value for key, reading in readings.items()}
    if "battery_pct" in values:
        values["battery_pct"] = int(values["battery_pct"])
    labels = {"battery_pct": "Батарейка", "temperature_c": "Температура", "humidity_pct": "Влажность"}
    caption = "Время получения показаний (UTC):\n" + "\n".join(
        f"{label}: " + (datetime.fromtimestamp(readings[key].received_at, timezone.utc)
                         .strftime("%Y-%m-%d %H:%M:%S") if key in readings else "нет показаний")
        for key, label in labels.items())
    return render(**values), caption
