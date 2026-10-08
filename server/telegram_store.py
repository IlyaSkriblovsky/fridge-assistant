"""Durable update claims and outgoing replies, never a conversation history."""

import sqlite3
from contextlib import closing

import metrics

UNCERTAIN = "Обработка запроса прервалась. Действие могло выполниться; проверь результат перед повтором."


def initialize(db):
    db.execute("""CREATE TABLE IF NOT EXISTS telegram_updates (
        bot_id INTEGER NOT NULL, update_id INTEGER NOT NULL,
        chat_id INTEGER NOT NULL, message_id INTEGER NOT NULL,
        state TEXT NOT NULL DEFAULT 'processing',
        text TEXT, document BLOB, caption TEXT, next_part INTEGER NOT NULL DEFAULT 0,
        PRIMARY KEY(bot_id, update_id))""")


def claim(bot_id, update_id, chat_id, message_id) -> bool:
    with closing(metrics.connect()) as db, db:
        return db.execute("""INSERT OR IGNORE INTO telegram_updates
            (bot_id, update_id, chat_id, message_id) VALUES (?, ?, ?, ?)""",
            (bot_id, update_id, chat_id, message_id)).rowcount == 1


def ready(bot_id, update_id, text=None, document=None, caption=None):
    with closing(metrics.connect()) as db, db:
        db.execute("""UPDATE telegram_updates SET state='ready', text=?, document=?, caption=?
            WHERE bot_id=? AND update_id=?""", (text, document, caption, bot_id, update_id))


def recover(bot_id):
    # A crash cannot tell us whether an external tool committed its action.
    with closing(metrics.connect()) as db, db:
        db.execute("""UPDATE telegram_updates SET state='ready', text=?
            WHERE bot_id=? AND state='processing'""", (UNCERTAIN, bot_id))


def pending(bot_id):
    with closing(metrics.connect()) as db:
        db.row_factory = sqlite3.Row
        return [dict(row) for row in db.execute("""SELECT * FROM telegram_updates
            WHERE bot_id=? AND state='ready' ORDER BY update_id""", (bot_id,))]


def advance(bot_id, update_id, next_part):
    with closing(metrics.connect()) as db, db:
        db.execute("UPDATE telegram_updates SET next_part=? WHERE bot_id=? AND update_id=?",
                   (next_part, bot_id, update_id))


def finish(bot_id, update_id):
    # Keep the deduplication key, discard delivered content (including PNGs).
    with closing(metrics.connect()) as db, db:
        db.execute("""UPDATE telegram_updates SET state='done', text=NULL, document=NULL, caption=NULL
            WHERE bot_id=? AND update_id=?""", (bot_id, update_id))
