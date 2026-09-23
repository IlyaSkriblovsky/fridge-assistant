# E4 -- AI button long press

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-15, with holds up to 20.4 s. No hardware long-press action observed; GPIO4 is usable for push-to-talk.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** The whole interaction is a long hold on GPIO4, which Seeed's own
documentation labels "AI/Power". If the hardware reacts to a long hold on its
own -- a power-off latch, for instance -- push-to-talk has to move to GPIO5 or
GPIO6 and the interaction changes.

**What to measure.** Whether the board survives a hold of 5, 15 and 30 seconds
with firmware that only reads the pin.

**How.** Falls out of the E1 rig as well: its second phase wakes on the AI
button and reports how long the hold lasted, doing nothing but reading the pin.
If the hardware reacts to a long hold, the board dies mid-report and the log
stops. The expectation is that nothing happens, since the stock firmware also
uses a long press, but it has not been confirmed on this unit.

**Result, 2026-09-15.** Twenty-four wakes on GPIO4 and one hold of 20.4 s with
firmware that does nothing but read the pin. The board stayed alive and kept
reporting every five seconds; nothing in hardware reacted, and no wake came back
as anything but `ext1` from `DEEPSLEEP`. **Push-to-talk can use GPIO4.**

Held to 20 s, not the 30 s the question asked for. For an interaction that lasts
seconds that is answered; the row stays open only for anyone who wants the last
ten seconds of it.

## Firmware button validation

Historical S2 checked 79 presses over three flashes. In the last 20, RTC-based
sleep-gap measurement distinguished a possible bounce wake (~60 ms) from a
fresh human press; the shortest observed gap was 196 ms and no press produced
two rows. Holds of 303/307 ms counted as questions, 272/282 ms as taps, and a
14-second hold completed normally.

Firmware hold duration starts at setup, missing about 60 ms of boot; a 300 ms
threshold therefore required roughly 360 ms of physical hold in that run.
The old single-task chirp also delayed button polling; that limitation ended
when polling moved into capture. Keep boot latency separate from debounce.
