# E3 -- Microphone settle window

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-15. Only the first 8 ms exceeds speech level; the configured settle discard was reduced from 200 ms to 24 ms. Further reduction needs a new measurement.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** `StickyMic::begin()` discards 200 ms of audio so the MEMS element and
the PDM-to-PCM filter can settle. The value was chosen with margin, not
measured, and it is paid directly out of the first word.

**What to measure.** With the mic rail just switched on, how long until the
block level stops reflecting the power-up transient and settles to the room
noise floor.

**How.** Falls out of the E1 rig, which starts the microphone with the settle
discard set to zero and prints the per-block RMS curve from the first block on,
in 8 ms blocks. It also reports where the curve first settles into a 3 dB band
around the tail, but the curve is printed so the criterion can be second-guessed.
A quiet room reads about -70 dBFS on this unit, which is the level to converge
to.

**Result, 2026-09-15.** Three curves from the E1 rig, quiet room, all the same
shape. In audio time -- block index times 8 ms, which is not the same as the
arrival timestamps the rig prints, since the DMA hands over 15 ms at a time:

| Audio time | Level |
| --- | --- |
| 0-8 ms | -31 dBFS |
| 8-24 ms | -45 to -58 dBFS |
| ~90 ms | within 10 dB of the floor |
| ~110-145 ms | within 6 dB |
| ~180-210 ms | within 3 dB |
| floor | -74 dBFS |

The 200 ms discard is not wrong about when the level stops moving. It is wrong
about what that has to do with speech. Speech at arm's length peaks near
-45 dBFS on this unit, so only the first 8 ms block is genuinely unusable -- it
is louder than speech. From 16 ms on the transient sits more than 13 dB below
speech, and from 90 ms more than 25 dB below, which is quieter than the room.

Two caveats on the rig rather than the microphone:

- The settle figure in the table is computed from arrival timestamps, so it is
  quantised to the 15 ms DMA buffer and reads up to 15 ms late.
- Its 3 dB criterion is tighter than the measurement noise: a single 8 ms RMS of
  room noise scatters over 5-6 dB, which is why the per-wake settle figure swings
  between 210 and 345 ms while the smoothed curve is the same every time. The
  numbers above come from a 5-block moving average of the printed curves.

**Acted on:** `StickyMic::kSettleMs` went from 200 ms to 24 ms -- three 8 ms
blocks, so the one block that is louder than speech is discarded with margin and
the rest of the transient goes to the backend more than 13 dB below the speech
it is under.

**Still open.** The decay is far too slow for a digital filter and slow even for
the MEMS element, which points at the bias settling as a decaying DC ramp --
per-block DC removal leaves the ramp inside the block, where it reads as level.
If that is what this is, a high-pass filter removes it outright and the discard
can go to nearly nothing. `readLevel()` already computes the per-block mean and
throws it away; reporting it alongside the RMS would settle the question in one
more run.

## Firmware capture validation

Retained from historical stages S3/S4. Two five-second captures each returned
80000 samples without short reads. The 960044-byte allocation was confirmed
in PSRAM. Speech and silence appeared at the expected locations; 100 ms blocks
showed the startup transient below speech at about 200 ms and at the room floor
by 400 ms. This coarse observation does not replace the fine-grained E3 settle
measurement above. Raw uploaded PCM retains DC bias; level measurements remove
it per block.

With capture moved to its own task, three recordings ran through a 2.60 s
full refresh and 0.68–1.27 s WiFi association:

| Held | Recorded | The task's own clock | Missing |
| --- | --- | --- | --- |
| 3832 ms | 3760 ms, 235 chunks | 3764 ms | 4 ms |
| 5032 ms | 4960 ms, 310 chunks | 4964 ms | 4 ms |
| 4342 ms | 4272 ms, 267 chunks | 4274 ms | 2 ms |

Elapsed time exceeded captured audio by only 2–4 ms, less than one chunk.
The longest read was 30.1 ms, and slow-read counts were 16, 21 and 18 versus
15.7, 20.7 and 17.8 expected from 256-sample reads crossing 240-frame DMA blocks.
A blocked read is not missing audio; time spent away from the DMA is the risk.
The later streaming validation also found elapsed/captured time within 7 ms
and one slow read per 14.3–15.7 reads. These small runs validate those workloads,
not every scheduling condition.
