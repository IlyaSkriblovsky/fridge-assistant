import asyncio
import sqlite3
import threading
from concurrent.futures import ThreadPoolExecutor
from unittest.mock import Mock

import pytest
from gkeepapi.node import List

import dashboard
import jobs
import metrics
import shopping_list

refresh = shopping_list.refresh


def test_persistence_and_source_isolation(monkeypatch):
    assert shopping_list.snapshot() is None
    metrics.save(shopping_list.metric_key(), 0)
    metrics.initialize()
    assert shopping_list.snapshot().value == 0
    monkeypatch.setenv("KEEP_NOTE_ID", "another-note")
    assert shopping_list.snapshot() is None


@pytest.fixture
def keep(monkeypatch):
    client = Mock()
    client.get.return_value = List()
    monkeypatch.setattr(shopping_list.gkeepapi, "Keep", lambda: client)
    return client


def test_refresh_and_voice_publish_actual_unchecked_count(keep):
    note = keep.get.return_value
    milk = note.add("Milk")
    note.add("Bread", checked=True)
    note.add("  ")
    refresh()
    assert shopping_list.snapshot().value == 1
    shopping_list.add(["Bread", "Eggs", "Milk"])
    assert shopping_list.snapshot().value == 3
    milk.checked = True
    refresh()
    assert shopping_list.snapshot().value == 2


def test_failed_sync_preserves_last_success(keep):
    metrics.save(shopping_list.metric_key(), 7)
    previous = shopping_list.snapshot()
    keep.sync.side_effect = OSError("offline")
    with pytest.raises(OSError):
        refresh()
    assert shopping_list.snapshot() == previous
    keep.sync.side_effect = [None, OSError("write failed")]
    assert "error" in shopping_list.add(["Milk"])
    assert shopping_list.snapshot() == previous


def test_db_failure_does_not_report_successful_edit_as_failure(keep, monkeypatch):
    monkeypatch.setattr(metrics, "save", Mock(side_effect=sqlite3.OperationalError("disk full")))
    assert shopping_list.add(["Milk"])["items"][0]["result"] == "added"
    assert keep.sync.call_count == 2


def test_refresh_cannot_overwrite_voice_edit(keep):
    entered, release = threading.Event(), threading.Event()
    def sync():
        entered.set()
        assert release.wait(2)
    keep.sync.side_effect = sync
    with ThreadPoolExecutor(max_workers=2) as executor:
        polling = executor.submit(refresh)
        assert entered.wait(2)
        edit = executor.submit(shopping_list.add, ["Milk"])
        release.set()
        polling.result(timeout=2)
        assert "items" in edit.result(timeout=2)
    assert shopping_list.snapshot().value == 1


@pytest.mark.asyncio
async def test_periodic_retries_after_error_and_stops():
    calls = 0
    done = asyncio.Event()
    loop = asyncio.get_running_loop()
    def action():
        nonlocal calls
        calls += 1
        if calls == 1:
            raise OSError("offline")
        loop.call_soon_threadsafe(done.set)
    task = asyncio.create_task(jobs.periodic("test", action, 0.01))
    try:
        await asyncio.wait_for(done.wait(), 2)
    finally:
        task.cancel()
        with pytest.raises(asyncio.CancelledError):
            await task
    assert calls == 2


@pytest.mark.asyncio
async def test_dashboard_reads_cached_value_without_keep(client):
    metrics.save(shopping_list.metric_key(), 12)
    response = await client.get("/sticky/dashboard")
    assert response.content == dashboard.png(dashboard.render(shopping=shopping_list.snapshot()))


def test_unknown_zero_and_stale_have_distinct_rendering(monkeypatch):
    monkeypatch.setattr(dashboard.time, "time", lambda: 5000)
    unknown = dashboard.png(dashboard.render())
    zero = dashboard.png(dashboard.render(shopping=metrics.Metric(0, 5000)))
    stale = dashboard.png(dashboard.render(shopping=metrics.Metric(0, 1000)))
    assert len({unknown, zero, stale}) == 3
