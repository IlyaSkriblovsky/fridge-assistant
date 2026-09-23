from datetime import datetime
from zoneinfo import ZoneInfo
from unittest.mock import Mock

import asyncio
import pytest

import jobs


@pytest.mark.parametrize("stamp,seconds", [
    ("2026-09-23T05:59:59", 1),
    ("2026-09-23T06:00:00", 0),
    ("2026-09-23T20:59:59", 0),
    ("2026-09-23T21:00:00", 9 * 3600),
    ("2026-09-23T23:00:00", 7 * 3600),
    ("2026-03-28T21:00:00", 8 * 3600),
    ("2026-10-24T21:00:00", 10 * 3600),
])
def test_night_delay(stamp, seconds):
    now = datetime.fromisoformat(stamp).replace(tzinfo=ZoneInfo("Asia/Nicosia"))
    assert jobs.night_delay(now) == seconds


@pytest.mark.asyncio
async def test_night_start_waits_until_morning(monkeypatch):
    clock = Mock()
    clock.now.return_value = datetime(2026, 9, 23, 21, tzinfo=ZoneInfo("Asia/Nicosia"))
    monkeypatch.setattr(jobs, "datetime", clock)
    action = Mock()
    waits = []

    async def sleep(delay):
        waits.append(delay)
        if len(waits) == 1:
            action.assert_not_called()
            clock.now.return_value = datetime(2026, 9, 24, 6, tzinfo=ZoneInfo("Asia/Nicosia"))
        else:
            raise asyncio.CancelledError

    monkeypatch.setattr(jobs.asyncio, "sleep", sleep)
    with pytest.raises(asyncio.CancelledError):
        await jobs.periodic("shopping", action, 900, daytime_only=True)
    action.assert_called_once()
    assert waits == [9 * 3600, 900]
