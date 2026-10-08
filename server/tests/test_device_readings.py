import sqlite3
from contextlib import closing

import pytest

import device_readings as readings
import metrics


def rows():
    with closing(metrics.connect()) as db:
        return db.execute('SELECT metric, value, received_at FROM device_readings ORDER BY id').fetchall()


def test_history_duplicates_partial_and_restart(monkeypatch):
    monkeypatch.setattr(readings.time, 'time', lambda: 1000.25)
    readings.save(70, -3.25, 45.75)
    readings.save(70, -3.25, 45.75)
    readings.save(80)
    readings.save()
    assert len(rows()) == 7
    assert {row[2] for row in rows()} == {1000.25}
    metrics.initialize()
    latest = readings.latest()
    assert latest['battery_pct'].value == 80
    assert latest['temperature_c'].value == -3.25
    assert latest['humidity_pct'].value == 45.75
    assert len(rows()) == 7
    monkeypatch.setattr(readings.time, 'time', lambda: 999)
    readings.save(5)
    assert readings.latest()['battery_pct'].value == 80


def test_atomic_write():
    with closing(metrics.connect()) as db, db:
        db.execute("""CREATE TRIGGER reject_sensor BEFORE INSERT ON device_readings
            WHEN NEW.metric='temperature_c' BEGIN SELECT RAISE(ABORT, 'test'); END""")
    with pytest.raises(sqlite3.IntegrityError):
        readings.save(50, 22, 60)
    assert rows() == []


@pytest.mark.asyncio
async def test_http_only_valid_authorized_readings(client):
    assert (await client.get('/sticky/dashboard')).status_code == 200
    assert (await client.get('/sticky/dashboard?battery_pct=50', headers={'Authorization': 'Bearer wrong'})).status_code == 401
    assert (await client.get('/sticky/dashboard?battery_pct=50&humidity_pct=101')).status_code == 422
    assert rows() == []
    assert (await client.get('/sticky/dashboard?battery_pct=50&temperature_c=21.75')).status_code == 200
    assert len(rows()) == 2
    assert rows()[0][2] == rows()[1][2]
    assert (await client.get('/sticky/dashboard?humidity_pct=60.25')).status_code == 200
    assert readings.latest()['temperature_c'].value == 21.75
    assert len(rows()) == 3
