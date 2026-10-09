"""Optional Telegram polling adapter; all assistant actions use the shared loop."""

import asyncio
import logging
import os
import re
from contextlib import AsyncExitStack, asynccontextmanager, suppress
from dataclasses import dataclass

import httpx
from telegram import ReplyParameters, Update
from telegram.ext import Application, TypeHandler

import assistant
import dashboard_service
import telegram_store as store

logger = logging.getLogger(__name__)
MAX_AUDIO_BYTES = 10 * 1024 * 1024
RETRY_SECONDS = 30
TYPING_SECONDS = 4


@dataclass(frozen=True)
class Config:
    token: str
    allowed_chats: frozenset[int]


def configuration() -> Config | None:
    token = os.environ.get("TELEGRAM_BOT_TOKEN", "").strip()
    if not token:
        return None
    raw = os.environ.get("TELEGRAM_ALLOWED_CHAT_IDS", "")
    parts = [part.strip() for part in raw.split(",")]
    if any(not re.fullmatch(r"-?[0-9]+", part) for part in parts):
        raise RuntimeError("TELEGRAM_ALLOWED_CHAT_IDS must contain comma-separated numeric Chat IDs")
    return Config(token, frozenset(int(part) for part in parts))


def split_text(text: str):
    """Bound UTF-16 units as well as characters, without losing any text."""
    chunk, units = [], 0
    for char in text:
        size = 2 if ord(char) > 0xFFFF else 1
        if units + size > 4096:
            yield "".join(chunk)
            chunk, units = [], 0
        chunk.append(char)
        units += size
    if chunk:
        yield "".join(chunk)


class AudioTooLarge(Exception):
    pass


class Adapter:
    def __init__(self, config, gemini, bot, downloads):
        self.config, self.gemini, self.bot, self.downloads = config, gemini, bot, downloads
        # Polling is sequential across all chats, including retries.
        self.lock = asyncio.Lock()

    async def keep_typing(self, chat_id):
        while True:
            try:
                await self.bot.send_chat_action(chat_id=chat_id, action="typing")
            except Exception as exc:
                # A cosmetic status must never prevent executing the request.
                logger.warning("[telegram] typing status failed: %s", type(exc).__name__)
            await asyncio.sleep(TYPING_SECONDS)

    @asynccontextmanager
    async def typing(self, chat_id):
        task = asyncio.create_task(self.keep_typing(chat_id))
        try:
            yield
        finally:
            await cancel(task)

    async def voice_bytes(self, voice):
        if (voice.file_size or 0) > MAX_AUDIO_BYTES:
            raise AudioTooLarge
        remote = await self.bot.get_file(voice.file_id)
        if (remote.file_size or 0) > MAX_AUDIO_BYTES:
            raise AudioTooLarge
        data = bytearray()
        async with self.downloads.stream("GET", remote.file_path) as response:
            response.raise_for_status()
            if int(response.headers.get("content-length", "0")) > MAX_AUDIO_BYTES:
                raise AudioTooLarge
            async for chunk in response.aiter_bytes(chunk_size=64 * 1024):
                if len(data) + len(chunk) > MAX_AUDIO_BYTES:
                    raise AudioTooLarge
                data.extend(chunk)
        return bytes(data)

    async def handle(self, update, context):
        chat = update.effective_chat
        if chat is None:
            return
        if chat.type != "private" or chat.id not in self.config.allowed_chats:
            logger.warning("[telegram] access denied: chat_id=%s chat_type=%s", chat.id, chat.type)
            return
        # Edited messages and channel posts never become new commands.
        message = update.message
        if message is None:
            return
        async with self.lock:
            claimed = await asyncio.to_thread(store.claim, self.bot.id, update.update_id, chat.id, message.message_id)
            if claimed:
                text, document, caption = None, None, None
                model_started = False
                try:
                    command = (message.text or "").split(maxsplit=1)[0] if message.text else ""
                    command = command.split("@", 1)[0]
                    if command == "/dashboard":
                        document, caption = await asyncio.to_thread(dashboard_service.telegram_snapshot)
                    elif command == "/start":
                        text = "Отправь текст или голосовое сообщение. /dashboard — текущий дашборд холодильника."
                    elif message.voice or message.text:
                        async with self.typing(chat.id):
                            payload = await self.voice_bytes(message.voice) if message.voice else message.text
                            mime = (message.voice.mime_type or "audio/ogg") if message.voice else "audio/wav"
                            model_started = True
                            text = await assistant.answer(self.gemini, payload,
                                lambda line: logger.info("[telegram] %s", line), mime_type=mime)
                    else:
                        text = "Отправь текст или голосовое сообщение."
                except AudioTooLarge:
                    text = "Слишком длинная запись (лимит 10 МБ)."
                except Exception as exc:
                    # Exception messages can contain the token-bearing download URL.
                    logger.warning("[telegram] request failed: %s", type(exc).__name__)
                    text = store.UNCERTAIN if model_started else "Не получилось загрузить сообщение или дашборд. Попробуй ещё раз."
                await asyncio.to_thread(store.ready, self.bot.id, update.update_id, text, document, caption)
            await self.deliver()

    async def deliver(self):
        blocked_chats = set()
        for row in await asyncio.to_thread(store.pending, self.bot.id):
            if row["chat_id"] not in self.config.allowed_chats or row["chat_id"] in blocked_chats:
                continue
            reply = ReplyParameters(message_id=row["message_id"], allow_sending_without_reply=True)
            try:
                if row["document"] is not None:
                    await self.bot.send_photo(chat_id=row["chat_id"], photo=bytes(row["document"]),
                        filename="dashboard.png", caption=row["caption"], parse_mode=None, reply_parameters=reply)
                else:
                    parts = list(split_text(row["text"] or assistant.NO_ANSWER))
                    for index in range(row["next_part"], len(parts)):
                        await self.bot.send_message(chat_id=row["chat_id"], text=parts[index],
                            parse_mode=None, reply_parameters=reply)
                        await asyncio.to_thread(store.advance, self.bot.id, row["update_id"], index + 1)
                await asyncio.to_thread(store.finish, self.bot.id, row["update_id"])
            except Exception as exc:
                logger.warning("[telegram] reply delivery failed: %s", type(exc).__name__)
                blocked_chats.add(row["chat_id"])

    async def retry(self):
        while True:
            try:
                async with self.lock:
                    await self.deliver()
            except Exception as exc:
                logger.warning("[telegram] retry failed: %s", type(exc).__name__)
            await asyncio.sleep(RETRY_SECONDS)


async def report_error(update, context):
    logger.warning("[telegram] polling/handler failed: %s", type(context.error).__name__)


async def cancel(task):
    task.cancel()
    with suppress(asyncio.CancelledError):
        await task


@asynccontextmanager
async def running(config, gemini):
    if config is None:
        yield
        return
    async with AsyncExitStack() as stack:
        application = Application.builder().token(config.token).concurrent_updates(False).build()
        await stack.enter_async_context(application)
        downloads = await stack.enter_async_context(httpx.AsyncClient(timeout=30))
        adapter = Adapter(config, gemini, application.bot, downloads)
        await asyncio.to_thread(store.recover, application.bot.id)
        application.add_handler(TypeHandler(Update, adapter.handle))
        application.add_error_handler(report_error)
        await application.start()
        stack.push_async_callback(application.stop)
        await application.updater.start_polling(allowed_updates=["message", "channel_post"],
                                               drop_pending_updates=False)
        stack.push_async_callback(application.updater.stop)
        retry = asyncio.create_task(adapter.retry())
        stack.push_async_callback(cancel, retry)
        yield
