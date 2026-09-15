# Experiments

Measurements this project needs but has not taken. Each one exists because a
decision depends on it and guessing would be worse than waiting.

Fill in **Result** and **Taken** when an experiment is run, and record what the
number changed. A measurement nobody acted on is worth noting too -- "measured,
no change needed" is a result.

| # | Question | Decision it unblocks | Status | Result |
| --- | --- | --- | --- | --- |
| E1 | How long from button press to the first usable audio sample? | Deep sleep vs light sleep | Not taken | -- |
| E2 | How much does HTTPS add to the round trip versus plain HTTP? | Whether the backend can live in the cloud | Not taken | -- |
| E3 | How long does the microphone actually need to settle? | How much of the first word is lost | Not taken | -- |
| E4 | Does holding the AI button trigger anything in hardware? | Whether push-to-talk can use GPIO4 at all | Not taken | -- |

---

## E1 -- Wake-to-first-sample latency

**Why.** Everything between the press and the first usable sample is speech the
device never hears. If it is small, deep sleep stays. If it is large, light sleep
becomes worth its higher idle draw, and the microphone would stay powered.

**What to measure.** From the ext1 wake event to the first sample the capture
path accepts, broken into:

1. ESP32-S3 boot -- image load from flash to the first line of `setup()`.
2. Power latch and mic rail (`StickyMic::begin()` currently waits 10 ms).
3. Microphone settle window (currently 200 ms of discarded audio -- see E3).

**How.** `esp_timer_get_time()` at the top of `setup()` gives time since reset,
which covers step 1 directly. Log a timestamp at each stage boundary over
Serial1 and wake the device from real deep sleep, not a reset -- the paths
differ.

**Watch for.** Measure after a genuine deep-sleep wake. A USB reset takes a
different path through the bootloader and will give an optimistic number.

## E2 -- HTTPS overhead

**Why.** The backend is meant to live outside the home network, which means TLS.
On a battery device that wakes for every question, the handshake is paid every
time. If it costs a second or more, either the backend moves to the LAN or a
local proxy becomes necessary -- both worse than cloud hosting, so it is worth
knowing the real number before deciding.

**What to measure.** Same request over `http://` and `https://`, comparing:

- Time from request start to the first response byte.
- Peak free heap during the request -- TLS buffers are tens of kilobytes and
  compete with the 960 KB audio buffer.

**How.** Same backend behind both schemes, same payload, from a cold wake.
Repeat enough times to see the spread; WiFi association time varies on its own
and must not be counted inside the measurement.

**Watch for.** Session resumption makes the second handshake much cheaper than
the first. Since every question starts from deep sleep, only the cold number
matters unless the session ticket is cached in RTC memory.

## E3 -- Microphone settle window

**Why.** `StickyMic::begin()` discards 200 ms of audio so the MEMS element and
the PDM-to-PCM filter can settle. The value was chosen with margin, not
measured, and it is paid directly out of the first word.

**What to measure.** With the mic rail just switched on, how long until the
block level stops reflecting the power-up transient and settles to the room
noise floor.

**How.** Drop the discard to zero, log per-block RMS from the first block, and
find where the curve flattens. A quiet room reads about -70 dBFS on this unit,
which is the level to converge to.

## E4 -- AI button long press

**Why.** The whole interaction is a long hold on GPIO4, which Seeed's own
documentation labels "AI/Power". If the hardware reacts to a long hold on its
own -- a power-off latch, for instance -- push-to-talk has to move to GPIO5 or
GPIO6 and the interaction changes.

**What to measure.** Whether the board survives a hold of 5, 15 and 30 seconds
with firmware that only reads the pin.

**How.** Falls out of the first build that reads the button; no separate rig
needed. The expectation is that nothing happens, since the stock firmware also
uses a long press, but it has not been confirmed on this unit.
