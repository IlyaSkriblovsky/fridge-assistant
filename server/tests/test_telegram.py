import asyncio
import io
from contextlib import closing
from types import SimpleNamespace
from unittest.mock import AsyncMock, Mock

import httpx
import pytest
from PIL import Image
from telegram import Update

import assistant
import dashboard_service
import device_readings
import metrics
import telegram_bot as tg
import telegram_store as store


def update(uid=1, text='молоко', chat_id=123, chat_type='private', voice=None):
    message = dict(message_id=uid + 100, date=1, chat=dict(id=chat_id, type=chat_type))
    if text is not None:
        message['text'] = text
    if voice:
        message['voice'] = voice
    return Update.de_json(dict(update_id=uid, message=message), None)


@pytest.fixture
def adapter(monkeypatch):
    monkeypatch.setattr(assistant, 'answer', AsyncMock(return_value='Готово'))
    bot = SimpleNamespace(id=42, send_message=AsyncMock(), send_document=AsyncMock(),
                          get_file=AsyncMock(return_value=SimpleNamespace(file_size=3, file_path='https://example.test/audio')))
    return tg.Adapter(tg.Config('dummy', frozenset({123})), object(), bot, None)


@pytest.mark.parametrize('raw', ['', ' ', '1,', 'foo', '1,1.5', '1 2'])
def test_invalid_configuration(monkeypatch, raw):
    monkeypatch.setenv('TELEGRAM_BOT_TOKEN', 'dummy')
    monkeypatch.setenv('TELEGRAM_ALLOWED_CHAT_IDS', raw)
    with pytest.raises(RuntimeError, match='TELEGRAM_ALLOWED_CHAT_IDS'):
        tg.configuration()


def test_configuration(monkeypatch):
    assert tg.configuration() is None
    monkeypatch.setenv('TELEGRAM_ALLOWED_CHAT_IDS', 'bad')
    assert tg.configuration() is None
    monkeypatch.setenv('TELEGRAM_BOT_TOKEN', 'dummy')
    monkeypatch.setenv('TELEGRAM_ALLOWED_CHAT_IDS', ' 123,456,123 ')
    assert tg.configuration().allowed_chats == {123, 456}


@pytest.mark.asyncio
async def test_disabled(monkeypatch):
    monkeypatch.setattr(tg.Application, 'builder', Mock(side_effect=AssertionError))
    async with tg.running(None, object()):
        pass


@pytest.mark.asyncio
@pytest.mark.parametrize('chat_id,chat_type', [(999, 'private'), (123, 'group'), (123, 'channel')])
@pytest.mark.parametrize('text', ['secret request', '/dashboard', '/start'])
async def test_denied(adapter, caplog, chat_id, chat_type, text):
    await adapter.handle(update(text=text, chat_id=chat_id, chat_type=chat_type,
                                voice=dict(file_id='secret-file', file_unique_id='u', duration=1)), None)
    assert f'chat_id={chat_id} chat_type={chat_type}' in caplog.text
    assert text not in caplog.text
    adapter.bot.get_file.assert_not_called()
    adapter.bot.send_message.assert_not_called()
    adapter.bot.send_document.assert_not_called()
    assistant.answer.assert_not_called()
    assert store.pending(42) == []


@pytest.mark.asyncio
async def test_text_reply_deduplicated_and_no_context(adapter):
    msg = update(text='добавь молоко')
    await adapter.handle(msg, None)
    await adapter.handle(msg, None)
    assistant.answer.assert_awaited_once()
    assert assistant.answer.call_args.args[1] == 'добавь молоко'
    kwargs = adapter.bot.send_message.call_args.kwargs
    assert kwargs['text'] == 'Готово'
    assert kwargs['chat_id'] == 123
    assert kwargs['reply_parameters'].message_id == 101
    assert kwargs['parse_mode'] is None
    adapter.bot.send_message.assert_awaited_once()
    metrics.initialize()
    store.recover(42)
    await adapter.handle(msg, None)
    assistant.answer.assert_awaited_once()


@pytest.mark.asyncio
async def test_voice_memory_and_mime(adapter):
    async with httpx.AsyncClient(transport=httpx.MockTransport(lambda req: httpx.Response(200, content=b'ogg'))) as downloads:
        adapter.downloads = downloads
        await adapter.handle(update(text=None, voice=dict(file_id='f', file_unique_id='u', duration=1, mime_type='audio/ogg')), None)
    assert assistant.answer.call_args.args[1] == b'ogg'
    assert assistant.answer.call_args.kwargs['mime_type'] == 'audio/ogg'


@pytest.mark.asyncio
@pytest.mark.parametrize('where', ['voice', 'file', 'header', 'stream'])
async def test_audio_limit(adapter, monkeypatch, where):
    monkeypatch.setattr(tg, 'MAX_AUDIO_BYTES', 2)
    voice = dict(file_id='f', file_unique_id='u', duration=1, file_size=3 if where == 'voice' else 1)
    adapter.bot.get_file.return_value.file_size = 3 if where == 'file' else 1
    def response(req):
        result = httpx.Response(200, content=b'123')
        if where == 'stream':
            result.headers.pop('content-length')
        return result
    async with httpx.AsyncClient(transport=httpx.MockTransport(response)) as downloads:
        adapter.downloads = downloads
        await adapter.handle(update(text=None, voice=voice), None)
    assistant.answer.assert_not_called()
    assert 'Слишком длинная' in adapter.bot.send_message.call_args.kwargs['text']
    if where == 'voice':
        adapter.bot.get_file.assert_not_called()


@pytest.mark.asyncio
async def test_send_failure_retry_and_restart(adapter):
    assistant.answer.return_value = '<b>literal</b>' + '😀' * 4096
    adapter.bot.send_message.side_effect = [None, httpx.ReadTimeout('secret-token')]
    await adapter.handle(update(), None)
    assert store.pending(42)[0]['next_part'] == 1
    metrics.initialize()
    store.recover(42)
    replacement = tg.Adapter(adapter.config, adapter.gemini, adapter.bot, None)
    adapter.bot.send_message.side_effect = None
    await replacement.handle(update(), None)
    assistant.answer.assert_awaited_once()
    delivered = [call.kwargs['text'] for index, call in enumerate(adapter.bot.send_message.call_args_list) if index != 1]
    assert ''.join(delivered) == assistant.answer.return_value
    assert all(len(s.encode('utf-16-le')) // 2 <= 4096 for s in delivered)
    assert store.pending(42) == []
    with closing(metrics.connect()) as db:
        assert db.execute('SELECT text, document FROM telegram_updates').fetchone() == (None, None)


@pytest.mark.asyncio
async def test_interrupted_request_never_reexecuted(adapter):
    store.claim(42, 1, 123, 101)
    metrics.initialize()
    store.recover(42)
    await adapter.handle(update(), None)
    assistant.answer.assert_not_called()
    assert adapter.bot.send_message.call_args.kwargs['text'] == store.UNCERTAIN


@pytest.mark.asyncio
async def test_model_failure_uncertain_and_no_retry(adapter, caplog):
    assistant.answer.side_effect = RuntimeError('secret-token')
    await adapter.handle(update(), None)
    await adapter.handle(update(), None)
    assistant.answer.assert_awaited_once()
    assert adapter.bot.send_message.call_args.kwargs['text'] == store.UNCERTAIN
    assert 'secret-token' not in caplog.text


@pytest.mark.asyncio
async def test_download_failure_before_model(adapter):
    adapter.bot.get_file.side_effect = httpx.ConnectError('offline')
    await adapter.handle(update(text=None, voice=dict(file_id='f', file_unique_id='u', duration=1)), None)
    assistant.answer.assert_not_called()
    assert 'Не получилось загрузить' in adapter.bot.send_message.call_args.kwargs['text']


@pytest.mark.asyncio
async def test_order(adapter):
    started, release = asyncio.Event(), asyncio.Event()
    calls = []
    async def answer(client, payload, log, **kwargs):
        calls.append(payload)
        if payload == 'first':
            started.set()
            await release.wait()
        return payload
    assistant.answer.side_effect = answer
    first = asyncio.create_task(adapter.handle(update(1, 'first'), None))
    await started.wait()
    second = asyncio.create_task(adapter.handle(update(2, 'second'), None))
    await asyncio.sleep(0)
    assert calls == ['first']
    release.set()
    await asyncio.gather(first, second)
    assert calls == ['first', 'second']
    assert [c.kwargs['text'] for c in adapter.bot.send_message.call_args_list] == calls


@pytest.mark.asyncio
@pytest.mark.parametrize('known', [False, True])
async def test_dashboard(adapter, monkeypatch, known):
    if known:
        monkeypatch.setattr(device_readings.time, 'time', lambda: 1000)
        device_readings.save(70, 20.5)
        monkeypatch.setattr(device_readings.time, 'time', lambda: 2000)
        device_readings.save(humidity_pct=60.5)
    before = device_readings.latest()
    expected, caption = dashboard_service.telegram_snapshot()
    await adapter.handle(update(text='/dashboard'), None)
    assistant.answer.assert_not_called()
    kwargs = adapter.bot.send_document.call_args.kwargs
    assert kwargs['document'] == expected
    assert kwargs['caption'] == caption
    assert kwargs['reply_parameters'].message_id == 101
    image = Image.open(io.BytesIO(expected))
    assert image.mode == '1' and image.size == (800, 480)
    assert device_readings.latest() == before
    if known:
        assert '00:16:40' in caption and '00:33:20' in caption
    else:
        assert caption.count('нет показаний') == 3


@pytest.mark.asyncio
async def test_removed_chat_not_retried(adapter):
    store.claim(42, 1, 999, 101)
    store.ready(42, 1, 'private reply')
    await adapter.deliver()
    adapter.bot.send_message.assert_not_called()


@pytest.mark.asyncio
@pytest.mark.parametrize('fail_polling', [False, True])
async def test_lifecycle(monkeypatch, fail_polling):
    events = []
    class FakeApplication:
        bot = SimpleNamespace(id=42)
        def __init__(self):
            self.updater = SimpleNamespace(start_polling=AsyncMock(side_effect=self.poll), stop=AsyncMock(side_effect=lambda: events.append('poll-stop')))
        async def __aenter__(self):
            events.append('initialize')
            return self
        async def __aexit__(self, *args):
            events.append('shutdown')
        async def start(self):
            events.append('start')
        async def stop(self):
            events.append('stop')
        async def poll(self, **kwargs):
            events.append('poll')
            assert kwargs['drop_pending_updates'] is False
            if fail_polling:
                raise RuntimeError('poll startup')
        def add_handler(self, handler):
            assert handler.callback.__self__.config.allowed_chats == {123}
        def add_error_handler(self, handler):
            pass
    application = FakeApplication()
    builder = Mock()
    builder.token.return_value = builder
    builder.concurrent_updates.return_value = builder
    builder.build.return_value = application
    monkeypatch.setattr(tg.Application, 'builder', Mock(return_value=builder))
    if fail_polling:
        with pytest.raises(RuntimeError, match='poll startup'):
            async with tg.running(tg.Config('dummy', frozenset({123})), object()):
                pytest.fail('must not enter')
        assert events == ['initialize', 'start', 'poll', 'stop', 'shutdown']
    else:
        async with tg.running(tg.Config('dummy', frozenset({123})), object()):
            assert events == ['initialize', 'start', 'poll']
        assert events == ['initialize', 'start', 'poll', 'poll-stop', 'stop', 'shutdown']
