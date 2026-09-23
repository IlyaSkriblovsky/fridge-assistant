"""Persistent last successful observations, not external service state."""

import os
import sqlite3
import time
from contextlib import closing
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class Metric:
    value: int
    updated_at: float


def connect() -> sqlite3.Connection:
    return sqlite3.connect(os.environ.get("DATABASE_PATH", "data/dashboard.sqlite3"), timeout=5)


def initialize() -> None:
    path = Path(os.environ.get("DATABASE_PATH", "data/dashboard.sqlite3"))
    path.parent.mkdir(parents=True, exist_ok=True)
    with closing(connect()) as db, db:
        db.execute("""CREATE TABLE IF NOT EXISTS metrics (
            key TEXT PRIMARY KEY, value INTEGER NOT NULL, updated_at REAL NOT NULL
        )""")


def save(key: str, value: int) -> None:
    with closing(connect()) as db, db:
        db.execute("""INSERT INTO metrics VALUES (?, ?, ?)
            ON CONFLICT(key) DO UPDATE SET
            value=excluded.value, updated_at=excluded.updated_at""",
            (key, value, time.time()))


def read(key: str) -> Metric | None:
    with closing(connect()) as db:
        row = db.execute("SELECT value, updated_at FROM metrics WHERE key=?", (key,)).fetchone()
    return Metric(*row) if row else None
