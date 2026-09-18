# /// script
# requires-python = ">=3.12"
# dependencies = ["gkeepapi>=0.17", "gpsoauth>=2"]
# ///
"""Try adding items to a Google Keep list through the unofficial gkeepapi.

The official Keep API only works in Google Workspace and cannot edit a note,
so this goes through gkeepapi, which talks to Keep's private sync API the way
the Android app does. It logs in with a master token, which is not an OAuth
token for one service: it stands for the whole Google account. Keep it secret.

The account is a separate one made for the assistant, with only the shopping
list shared with it. The sync API cannot fetch a single note, only everything
that changed since a version the client already has. For this account
"everything" is that one list, so each run syncs from scratch and nothing is
kept on disk.

Getting a master token (once):
  1. Open https://accounts.google.com/EmbeddedSetup in a private window, sign
     in as the assistant's account and press "I agree". The page may keep
     spinning afterwards, that is fine.
  2. In the browser's developer tools, copy the value of the `oauth_token`
     cookie for accounts.google.com. It starts with `oauth2_4/` and works once.
  3. uv run experiments/keep_list.py token assistant@gmail.com
     paste the cookie when asked; the script prints a token starting with
     `aas_et/`.

Then put KEEP_EMAIL and KEEP_MASTER_TOKEN into .env and:
    uv run --env-file .env experiments/keep_list.py lists          # every list
    uv run --env-file .env experiments/keep_list.py show Покупки   # its items
    uv run --env-file .env experiments/keep_list.py add Покупки молоко "кошачий корм"

A list is named by its title or its id, which `lists` prints. Every command
prints how long login and sync took.
"""

from __future__ import annotations

import argparse
import getpass
import os
import secrets
import sys
import time

import gkeepapi
import gpsoauth
from gkeepapi import exception
from gkeepapi.node import List, NewListItemPlacementValue


def get_master_token(email: str) -> int:
    oauth_token = getpass.getpass("oauth_token cookie (input hidden): ").strip()
    # Any 16 hex digits do: Google only needs something that looks like a device.
    android_id = secrets.token_hex(8)
    response = gpsoauth.exchange_token(email, oauth_token, android_id)
    if "Token" not in response:
        print(f"Exchange failed: {response}", file=sys.stderr)
        return 1
    print(response["Token"])
    return 0


def login() -> gkeepapi.Keep:
    email = os.environ.get("KEEP_EMAIL")
    master_token = os.environ.get("KEEP_MASTER_TOKEN")
    if not (email and master_token):
        sys.exit("Set KEEP_EMAIL and KEEP_MASTER_TOKEN (see the script's docstring)")

    keep = gkeepapi.Keep()
    started = time.monotonic()
    keep.authenticate(email, master_token, sync=False)
    print(f"login: {time.monotonic() - started:.2f} s")

    started = time.monotonic()
    keep.sync()
    print(f"full sync: {time.monotonic() - started:.2f} s, {len(keep.all())} notes\n")
    return keep


def list_notes(keep: gkeepapi.Keep) -> list[List]:
    return [note for note in keep.all() if isinstance(note, List) and not note.trashed]


def find_list(keep: gkeepapi.Keep, name: str) -> List:
    matches = [
        note
        for note in list_notes(keep)
        if note.id == name or note.title.casefold() == name.casefold()
    ]
    if len(matches) == 1:
        return matches[0]
    if not matches:
        titles = ", ".join(repr(n.title) for n in list_notes(keep)) or "none"
        sys.exit(f"No list {name!r}. Lists: {titles}")
    sys.exit(f"{len(matches)} lists are titled {name!r}, use the id")


def show_lists(keep: gkeepapi.Keep) -> None:
    for note in list_notes(keep):
        collaborators = note.collaborators.all()
        shared = f", shared with {len(collaborators)}" if collaborators else ""
        archived = ", archived" if note.archived else ""
        print(
            f"{note.title!r} ({note.id}): {len(note.unchecked)} to buy, "
            f"{len(note.checked)} ticked{shared}{archived}  "
            f"(edited {note.timestamps.edited:%Y-%m-%d %H:%M})"
        )


def show_items(note: List) -> None:
    print(f"{note.title!r}")
    for item in note.items:
        mark = "x" if item.checked else " "
        print(f"  [{mark}] {item.text}")


def add_items(keep: gkeepapi.Keep, note: List, texts: list[str]) -> None:
    on_list = {item.text.casefold() for item in note.unchecked}
    for text in texts:
        if text.casefold() in on_list:
            print(f"  note: {text!r} is already on the list, adding it again")
        # Keep's own default setting puts new items at the bottom.
        note.add(text, sort=NewListItemPlacementValue.Bottom)

    started = time.monotonic()
    keep.sync()
    print(f"pushed {len(texts)} item(s) in {time.monotonic() - started:.2f} s\n")
    show_items(note)


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Try Google Keep lists through gkeepapi.",
        epilog="Setup and examples are in the script's docstring.",
    )
    commands = parser.add_subparsers(dest="command", required=True)
    token = commands.add_parser("token", help="exchange an oauth_token cookie for a master token")
    token.add_argument("email")
    commands.add_parser("lists", help="show every list note")
    show = commands.add_parser("show", help="show the items of one list")
    show.add_argument("list", help="title or id")
    add = commands.add_parser("add", help="add items to a list")
    add.add_argument("list", help="title or id")
    add.add_argument("items", nargs="+")
    args = parser.parse_args()

    if args.command == "token":
        return get_master_token(args.email)

    try:
        keep = login()
        if args.command == "lists":
            show_lists(keep)
        elif args.command == "show":
            show_items(find_list(keep, args.list))
        elif args.command == "add":
            add_items(keep, find_list(keep, args.list), args.items)
    except exception.LoginException as exc:
        print(f"Login failed: {exc}", file=sys.stderr)
        return 1
    except exception.APIException as exc:
        print(f"Keep API error: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
