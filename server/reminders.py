"""Persistent one-shot reminders and server-owned calendar arithmetic."""
from calendar import monthrange
from contextlib import closing
from contextvars import ContextVar
from datetime import datetime, timedelta, timezone
from zoneinfo import ZoneInfo
import os
import time
import re

import metrics

ZONE = ZoneInfo(os.environ.get("REMINDERS_TIMEZONE", "Asia/Nicosia"))
REQUEST_TIME: ContextVar[datetime | None] = ContextVar("reminder_request_time", default=None)
MAX_ACTIVE = 10
MAX_TEXT_BYTES = 240
MAX_TIME_MS = 4102444800000  # 2100-01-01 UTC; shared with firmware.


def initialize(db):
    db.execute("""CREATE TABLE IF NOT EXISTS reminders (
        id INTEGER PRIMARY KEY AUTOINCREMENT, text TEXT NOT NULL,
        due_at INTEGER NOT NULL, created_at INTEGER NOT NULL,
        status TEXT NOT NULL CHECK(status IN ('active','cancelled','read')))
    """)
    db.execute("CREATE TABLE IF NOT EXISTS reminder_version (id INTEGER PRIMARY KEY CHECK(id=1), version INTEGER NOT NULL)")
    db.execute("INSERT OR IGNORE INTO reminder_version VALUES (1, 0)")


def local_time(value: datetime) -> datetime:
    """Reject both folds and gaps instead of silently choosing a DST offset."""
    candidates = set()
    for fold in (0, 1):
        utc = value.replace(tzinfo=ZONE, fold=fold).astimezone(timezone.utc)
        if utc.astimezone(ZONE).replace(tzinfo=None) == value:
            candidates.add(utc)
    if len(candidates) != 1:
        raise ValueError("Уточни время: оно неоднозначно или не существует при переводе часов.")
    return candidates.pop()


def deadline(reference: datetime, *, amount=None, unit=None, local_at=None) -> datetime:
    if local_at is not None:
        if amount is not None or unit is not None or not isinstance(local_at, str):
            raise ValueError("Укажи либо местную дату и время, либо длительность.")
        # Require explicit date AND time, with no timezone supplied by the model.
        if not re.fullmatch(r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}(?::\d{2})?", local_at):
            raise ValueError("Нужна точная местная дата и время YYYY-MM-DDTHH:MM:SS.")
        due = local_time(datetime.fromisoformat(local_at))
    else:
        if type(amount) is not int or not 1 <= amount <= 1000000:
            raise ValueError("Длительность должна быть положительным целым числом.")
        if unit in ("seconds", "minutes", "hours"):
            due = reference + timedelta(seconds=amount * {"seconds": 1, "minutes": 60, "hours": 3600}[unit])
        elif unit in ("days", "weeks", "months", "years"):
            local = reference.astimezone(ZONE).replace(tzinfo=None)
            if unit in ("days", "weeks"):
                local += timedelta(days=amount * (7 if unit == "weeks" else 1))
            else:
                months = local.year * 12 + local.month - 1 + amount * (12 if unit == "years" else 1)
                year, month = divmod(months, 12)
                local = local.replace(year=year, month=month + 1,
                                      day=min(local.day, monthrange(year, month + 1)[1]))
            due = local_time(local)
        else:
            raise ValueError("Неизвестная единица длительности.")
    if due <= reference or not 0 < due.timestamp() * 1000 < MAX_TIME_MS:
        raise ValueError("Срок должен быть в будущем, до 2100 года.")
    return due


def bump(db):
    db.execute("UPDATE reminder_version SET version=version+1 WHERE id=1")


def create(text, amount=None, unit=None, local_at=None):
    try:
        if (not isinstance(text, str) or not text.strip()
                or any(ord(c) < 32 or ord(c) == 127 for c in text)
                or len(text.encode('utf-8')) > MAX_TEXT_BYTES):
            raise ValueError(f"Текст должен содержать от 1 до {MAX_TEXT_BYTES} байт UTF-8 без управляющих символов.")
        text = text.strip()
        text = text[:1].upper() + text[1:]
        if len(text.encode('utf-8')) > MAX_TEXT_BYTES:
            raise ValueError(f"Текст должен содержать не более {MAX_TEXT_BYTES} байт UTF-8 после преобразования регистра.")
        reference = REQUEST_TIME.get() or datetime.now(timezone.utc)
        due = deadline(reference, amount=amount, unit=unit, local_at=local_at)
        with closing(metrics.connect()) as db, db:
            db.execute("BEGIN IMMEDIATE")
            if db.execute("SELECT count(*) FROM reminders WHERE status='active'").fetchone()[0] >= MAX_ACTIVE:
                return {"error": "Уже есть 10 активных напоминаний. Прочитай или отмени одно из них."}
            cursor = db.execute("INSERT INTO reminders(text,due_at,created_at,status) VALUES (?,?,?,'active')",
                                (text, int(due.timestamp() * 1000), int(time.time() * 1000)))
            bump(db)
            return {"id": cursor.lastrowid, "text": text, "due_at": due.astimezone(ZONE).isoformat()}
    except (ValueError, OverflowError) as exc:
        return {"error": str(exc)}


def cancel(id):
    if type(id) is not int or not 0 < id <= 9007199254740991:
        return {"error": "Нужен положительный ID из списка напоминаний."}
    with closing(metrics.connect()) as db, db:
        db.execute("BEGIN IMMEDIATE")
        changed = db.execute("UPDATE reminders SET status='cancelled' WHERE id=? AND status='active'", (id,)).rowcount
        if changed:
            bump(db)
        return {"id": id, "cancelled": bool(changed)}


def snapshot(read_ids=()):
    """Acknowledgements and snapshot share a transaction and durable version."""
    with closing(metrics.connect()) as db, db:
        db.execute("BEGIN IMMEDIATE")
        changed = 0
        for id in read_ids:
            changed += db.execute("UPDATE reminders SET status='read' WHERE id=? AND status='active'", (id,)).rowcount
        if changed:
            bump(db)
        rows = db.execute("SELECT id,text,due_at,created_at FROM reminders WHERE status='active' ORDER BY due_at,created_at,id").fetchall()
        version = db.execute("SELECT version FROM reminder_version WHERE id=1").fetchone()[0]
        return {"version": version, "server_time_ms": int(time.time() * 1000),
                "active": [dict(zip(('id', 'text', 'due_at_ms', 'created_at_ms'), row)) for row in rows],
                "acknowledged_ids": list(dict.fromkeys(read_ids))}


def list_active():
    return {"reminders": [{**row, "due_at": datetime.fromtimestamp(row['due_at_ms'] / 1000, timezone.utc).astimezone(ZONE).isoformat()}
                          for row in snapshot()['active']]}
