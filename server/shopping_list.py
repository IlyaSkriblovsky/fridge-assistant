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

import os

import gkeepapi
from gkeepapi.exception import APIException, KeepException
from gkeepapi.node import List, ListItem, NewListItemPlacementValue

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


def put(note: List, text: str) -> str:
    """Make sure one item is on the list, unticked, and say how it got there."""
    if find(note.unchecked, text) is not None:
        return ALREADY_LISTED
    ticked = find(note.checked, text)
    if ticked is not None:
        # Ticked off on an earlier trip: bringing it back keeps the list from
        # growing a second copy under the ticked ones.
        ticked.checked = False
        return RESTORED
    # Keep's own default setting puts new items at the bottom.
    note.add(" ".join(text.split()), sort=NewListItemPlacementValue.Bottom)
    return ADDED


def add(items: list[str]) -> dict[str, object]:
    """Put items onto the shopping list. Returns what happened to each, or an
    error the model can tell the user about."""
    texts = [text for text in items if text.strip()]
    if not texts:
        return {"error": "no items given"}

    try:
        keep = gkeepapi.Keep()
        keep.authenticate(
            os.environ["KEEP_EMAIL"], os.environ["KEEP_MASTER_TOKEN"], sync=False
        )
        keep.sync()
        note = keep.get(os.environ["KEEP_NOTE_ID"])
        if not isinstance(note, List) or note.trashed:
            return {"error": "the shopping list is missing from Google Keep"}

        # One at a time against the live note, so an item named twice in one
        # request is caught as already listed the second time.
        results = [{"item": text, "result": put(note, text)} for text in texts]
        keep.sync()
    # requests' errors, which the network ones arrive as, are OSErrors.
    except (KeepException, APIException, OSError) as exc:
        return {"error": f"Google Keep failed: {exc}"}
    return {"items": results}
