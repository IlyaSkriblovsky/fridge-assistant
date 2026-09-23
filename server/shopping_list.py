"""The family's shopping list, a Google Keep list note, through gkeepapi.

gkeepapi talks to Keep's private sync API the way the Android app does, since
the official Keep API cannot edit a note. It logs in as the assistant's own
Google account, which has only the shopping list shared with it: the master
token stands for the whole account, so the family's own accounts stay out of
reach.

The sync API cannot fetch a single note, only everything that changed since a
version the client already has. For this account everything is the one list,
so every call logs in and syncs from scratch and nothing is kept between
requests.

gkeepapi is synchronous; call these from a worker thread.
"""

from __future__ import annotations

import logging
import os
import sqlite3
import threading

import gkeepapi
from gkeepapi.exception import APIException, KeepException
from gkeepapi.node import List, ListItem, NewListItemPlacementValue

import metrics

REFRESH_SECONDS = 900
_lock = threading.Lock()
logger = logging.getLogger(__name__)


def metric_key() -> str:
    return "shopping-count:" + os.environ["KEEP_NOTE_ID"]


def snapshot() -> metrics.Metric | None:
    return metrics.read(metric_key())


def publish(note: List) -> None:
    metrics.save(metric_key(), sum(bool(item.text.strip()) for item in note.unchecked))


def refresh() -> None:
    # Serialize full sync/read/write cycles with voice edits, not just DB writes.
    if not _lock.acquire(blocking=False):
        return
    try:
        _, note = load()
        publish(note)
    finally:
        _lock.release()


def load() -> tuple[gkeepapi.Keep, List]:
    keep = gkeepapi.Keep()
    keep.authenticate(os.environ["KEEP_EMAIL"], os.environ["KEEP_MASTER_TOKEN"], sync=False)
    keep.sync()
    note = keep.get(os.environ["KEEP_NOTE_ID"])
    if not isinstance(note, List) or note.trashed:
        raise ValueError("the shopping list is missing from Google Keep")
    return keep, note


# What happened to each item, as the model gets it back.
ADDED = "added"
ALREADY_LISTED = "already_listed"
RESTORED = "restored"


def normalize(text: str) -> str:
    """The form two items are compared in: case, and spaces at the ends and
    doubled inside, make no difference. Anything else does, so "молоко" and
    "молоко 2 л" are two different items."""
    return " ".join(text.split()).casefold()


def find(items: list[ListItem], text: str) -> ListItem | None:
    key = normalize(text)
    return next((item for item in items if normalize(item.text) == key), None)


def capitalize_item(text: str) -> str:
    """Uppercase the first letter, preserving brands and units in the rest."""
    text = " ".join(text.split())
    for index, char in enumerate(text):
        if char.isalpha():
            return text[:index] + char.upper() + text[index + 1:]
    return text


def put(note: List, text: str) -> str:
    """Make sure one item is on the list, unticked, and say how it got there."""
    existing = find(note.unchecked, text)
    if existing is not None:
        existing.text = capitalize_item(existing.text)
        return ALREADY_LISTED
    ticked = find(note.checked, text)
    if ticked is not None:
        # Ticked off on an earlier trip: bringing it back keeps the list from
        # growing a second copy under the ticked ones.
        ticked.text = capitalize_item(ticked.text)
        ticked.checked = False
        return RESTORED
    # Keep's own default setting puts new items at the bottom.
    note.add(capitalize_item(text), sort=NewListItemPlacementValue.Bottom)
    return ADDED


def add(items: list[str]) -> dict[str, object]:
    if not _lock.acquire(timeout=10):
        return {"error": "the shopping list is busy; try again shortly"}
    try:
        return _add(items)
    finally:
        _lock.release()


def _add(items: list[str]) -> dict[str, object]:
    """Put items onto the shopping list. Returns what happened to each, or an
    error the model can tell the user about."""
    texts = [text for text in items if text.strip()]
    if not texts:
        return {"error": "no items given"}

    try:
        keep, note = load()

        # One at a time against the live note, so an item named twice in one
        # request is caught as already listed the second time.
        results = [{"item": text, "result": put(note, text)} for text in texts]
        keep.sync()
    except ValueError as exc:
        return {"error": str(exc)}
    # requests' errors, which the network ones arrive as, are OSErrors.
    except (KeepException, APIException, OSError) as exc:
        return {"error": f"Google Keep failed: {exc}"}
    try:
        publish(note)
    except (sqlite3.Error, OSError):
        # The edit already succeeded. Do not invite the model to repeat it.
        logger.exception("Keep updated, but saving the dashboard count failed")
    return {"items": results}
