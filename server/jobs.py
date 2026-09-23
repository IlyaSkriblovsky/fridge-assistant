"""In-process periodic jobs for a single server worker."""

import asyncio
import logging
from collections.abc import Callable
from datetime import datetime, timedelta
from zoneinfo import ZoneInfo

logger = logging.getLogger(__name__)


def night_delay(now: datetime) -> float:
    """Seconds until 05:00 local time, or zero during the polling window."""
    if 5 <= now.hour < 21:
        return 0
    morning = now.replace(hour=5, minute=0, second=0, microsecond=0)
    if now.hour >= 21:
        morning += timedelta(days=1)
    # Timestamps account for DST changes during the night.
    return morning.timestamp() - now.timestamp()


async def periodic(
    name: str, action: Callable[[], object], interval: float,
    *, daytime_only: bool = False,
) -> None:
    """Run in the allowed window, then wait; never overlap a job with itself."""
    while True:
        if daytime_only:
            delay = night_delay(datetime.now(ZoneInfo("Asia/Nicosia")))
            if delay:
                await asyncio.sleep(delay)
                continue
        try:
            await asyncio.to_thread(action)
        except Exception:
            logger.exception("Background job %s failed; retaining the last snapshot", name)
        await asyncio.sleep(interval)
