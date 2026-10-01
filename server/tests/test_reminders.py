from concurrent.futures import ThreadPoolExecutor
from datetime import datetime, timezone
from unittest.mock import Mock
from zoneinfo import ZoneInfo, ZoneInfoNotFoundError
import runpy

import pytest
from google.genai import types

import assistant
import metrics
import reminders
from test_assistant import response


def dt(value):
    return datetime.fromisoformat(value).replace(tzinfo=timezone.utc)


@pytest.mark.parametrize('zone', [None, 'America/New_York'])
def test_timezone_configuration(monkeypatch, zone):
    monkeypatch.delenv('REMINDERS_TIMEZONE', raising=False)
    if zone is not None:
        monkeypatch.setenv('REMINDERS_TIMEZONE', zone)
    configured = runpy.run_path(reminders.__file__)
    assert configured['ZONE'].key == (zone or 'Asia/Nicosia')
    if zone is None:
        return
    token = configured['REQUEST_TIME'].set(dt('2026-03-07T15:00:00'))
    try:
        created = configured['create']('test', amount=1, unit='days')
        assert created['due_at'] == '2026-03-08T10:00:00-04:00'
        assert configured['list_active']()['reminders'][0]['due_at'] == created['due_at']
        assert configured['deadline'](dt('2026-03-07'), local_at='2026-03-08T10:00:00') == dt('2026-03-08T14:00:00')
        for local in ['2026-03-08T02:30:00', '2026-11-01T01:30:00']:
            with pytest.raises(ValueError):
                configured['deadline'](dt('2026-01-01'), local_at=local)
    finally:
        configured['REQUEST_TIME'].reset(token)


@pytest.mark.parametrize('zone', ['', 'Invalid/Timezone'])
def test_invalid_timezone_rejected(monkeypatch, zone):
    monkeypatch.setenv('REMINDERS_TIMEZONE', zone)
    with pytest.raises((ValueError, ZoneInfoNotFoundError)):
        runpy.run_path(reminders.__file__)


def test_calendar_and_duration():
    reference = dt('2026-01-31T16:00:00')
    assert reminders.deadline(reference, amount=1, unit='months') == dt('2026-02-28T16:00:00')
    assert reminders.deadline(dt('2024-02-29T16:00:00'), amount=1, unit='years') == dt('2025-02-28T16:00:00')
    assert reminders.deadline(reference, amount=5, unit='minutes') == dt('2026-01-31T16:05:00')
    assert reminders.deadline(dt('2026-09-24T15:00:00'), amount=1, unit='months') == dt('2026-10-24T15:00:00')
    # Calendar days preserve local wall time across the spring transition.
    assert reminders.deadline(dt('2026-03-28T10:00:00'), amount=1, unit='days') == dt('2026-03-29T09:00:00')


@pytest.mark.parametrize('local', ['2026-03-29T03:30:00', '2026-10-25T03:30:00', '2026-10-25', '2026-10-25T18:00:00+03:00'])
def test_dst_and_incomplete_calendar_rejected(local):
    with pytest.raises(ValueError):
        reminders.deadline(dt('2026-01-01'), local_at=local)


@pytest.mark.parametrize('args', [dict(amount=True, unit='minutes'), dict(amount=-1, unit='days'),
    dict(amount=1.5, unit='months'), dict(amount=1, unit='unknown'),
    dict(amount=1, unit='months', local_at='2026-10-25T18:00:00'), dict(local_at='2000-01-01T10:00:00'),
    dict(amount=1000000, unit='years')])
def test_bad_deadlines(args):
    assert 'error' in reminders.create('тест', **args)


def test_atomic_limit_and_persistence():
    with ThreadPoolExecutor(max_workers=15) as pool:
        results = list(pool.map(lambda i: reminders.create(str(i), amount=5, unit='minutes'), range(30)))
    assert sum('id' in row for row in results) == 10
    first = reminders.snapshot()
    assert first['version'] == 10
    assert len(first['active']) == 10
    metrics.initialize()
    assert reminders.snapshot()['active'] == first['active']
    assert reminders.snapshot()['version'] == first['version']
    id = first['active'][0]['id']
    assert reminders.cancel(id)['cancelled']
    assert not reminders.cancel(id)['cancelled']
    assert 'id' in reminders.create('replacement', amount=1, unit='seconds')
    assert reminders.snapshot()['version'] == 12


def test_ack_loss_repeat_and_cancelled():
    a = reminders.create('a', amount=1, unit='seconds')['id']
    b = reminders.create('b', amount=1, unit='seconds')['id']
    reminders.cancel(b)
    first = reminders.snapshot([a, b, 999])
    again = reminders.snapshot([a, b, 999])
    assert first['version'] == again['version'] == 4
    assert again['active'] == []
    assert again['acknowledged_ids'] == [a, b, 999]


def test_text_limit_no_truncation():
    assert 'id' in reminders.create('я' * 120, amount=1, unit='minutes')
    for text in ['я' * 121, '', '  ', 'a\x00b', None]:
        assert 'error' in reminders.create(text, amount=1, unit='minutes')


@pytest.mark.asyncio
async def test_audio_create_list_cancel(client, gemini, wav):
    gemini.aio.models.generate_content.side_effect = [
        response(types.Part(function_call=types.FunctionCall(name='create_reminder', args={'text': 'Яйца', 'amount': 5, 'unit': 'minutes'}))),
        response(types.Part(text='Напомню.'))]
    before = datetime.now(timezone.utc).timestamp() * 1000
    reply = (await client.post('/audio', content=wav)).json()
    snap = reply['reminders']
    assert reply['response'] == 'Напомню.'
    assert int(before) + 300000 <= snap['active'][0]['due_at_ms'] <= snap['server_time_ms'] + 300000
    id = snap['active'][0]['id']
    gemini.aio.models.generate_content.side_effect = [
        response(types.Part(function_call=types.FunctionCall(name='list_reminders', args={}))),
        response(types.Part(function_call=types.FunctionCall(name='cancel_reminder', args={'id': id}))),
        response(types.Part(text='Отменено.'))]
    cancelled = (await client.post('/audio', content=wav)).json()['reminders']
    assert cancelled['active'] == []
    assert cancelled['version'] > snap['version']


@pytest.mark.asyncio
async def test_sync_auth_validation_and_ack(client):
    id = reminders.create('test', amount=1, unit='seconds')['id']
    assert (await client.post('/sticky/reminders/sync', json={'read_ids': []}, headers={'Authorization': ''})).status_code == 401
    for ids in [[True], [0], ['1'], list(range(1, 12))]:
        assert (await client.post('/sticky/reminders/sync', json={'read_ids': ids})).status_code == 422
    a = (await client.post('/sticky/reminders/sync', json={'read_ids': [id]})).json()
    b = (await client.post('/sticky/reminders/sync', json={'read_ids': [id]})).json()
    assert a['version'] == b['version']
    assert b['acknowledged_ids'] == [id]
    assert b['active'] == []


@pytest.mark.asyncio
@pytest.mark.parametrize('zone, local_time', [
    ('Asia/Nicosia', '2026-09-24T18:00:00+03:00'),
    ('America/New_York', '2026-09-24T11:00:00-04:00'),
])
async def test_fixed_anchor_across_model_rounds(gemini, monkeypatch, zone, local_time):
    monkeypatch.setattr(reminders, 'ZONE', ZoneInfo(zone))
    anchor = dt('2026-09-24T15:00:00')
    token = reminders.REQUEST_TIME.set(anchor)
    seen = []
    replies = iter([
        response(types.Part(function_call=types.FunctionCall(name='create_reminder', args={'text': 'a', 'amount': 5, 'unit': 'minutes'}))),
        response(types.Part(function_call=types.FunctionCall(name='create_reminder', args={'text': 'b', 'amount': 5, 'unit': 'minutes'}))),
        response(types.Part(text='Done'))])
    async def generate(**kwargs):
        seen.append(kwargs['config'].system_instruction)
        return next(replies)
    gemini.aio.models.generate_content.side_effect = generate
    try:
        await assistant.answer(gemini, b'audio', Mock())
    finally:
        reminders.REQUEST_TIME.reset(token)
    rows = reminders.snapshot()['active']
    assert rows[0]['due_at_ms'] == rows[1]['due_at_ms'] == int(anchor.timestamp() * 1000) + 300000
    assert all(f'{local_time} ({zone})' in instruction for instruction in seen)
