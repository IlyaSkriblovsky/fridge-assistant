"""Append-only device observations in the shared SQLite database."""

import time
from contextlib import closing
from dataclasses import dataclass

import metrics

METRICS = ("battery_pct", "temperature_c", "humidity_pct")


@dataclass(frozen=True)
class Reading:
    value: float
    received_at: float


def initialize(db):
    db.execute("""CREATE TABLE IF NOT EXISTS device_readings (
        id INTEGER PRIMARY KEY, metric TEXT NOT NULL
        CHECK(metric IN ('battery_pct', 'temperature_c', 'humidity_pct')),
        value REAL NOT NULL, received_at REAL NOT NULL)""")
    db.execute("""CREATE INDEX IF NOT EXISTS device_readings_latest
        ON device_readings(metric, received_at DESC, id DESC)""")
    db.execute("""CREATE INDEX IF NOT EXISTS device_readings_time
        ON device_readings(received_at)""")


def save(battery_pct=None, temperature_c=None, humidity_pct=None):
    now = time.time()
    rows = [(key, value, now) for key, value in
            zip(METRICS, (battery_pct, temperature_c, humidity_pct)) if value is not None]
    if rows:
        with closing(metrics.connect()) as db, db:
            db.executemany("INSERT INTO device_readings(metric, value, received_at) VALUES (?, ?, ?)", rows)


def latest() -> dict[str, Reading]:
    with closing(metrics.connect()) as db, db:
        db.execute("BEGIN")
        result = {}
        for key in METRICS:
            row = db.execute("""SELECT value, received_at FROM device_readings
                WHERE metric=? ORDER BY received_at DESC, id DESC LIMIT 1""", (key,)).fetchone()
            if row:
                result[key] = Reading(*row)
        return result
