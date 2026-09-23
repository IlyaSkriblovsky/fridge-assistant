# E8 -- What a refresh is made of

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-17. Removed 100 ms of redundant driver sleep per refresh. Display work now runs on its own task; controller queueing remains. Historical timing includes different drawing workloads; distinguish refresh-only and whole-screen measurements.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** [S5](#firmware-display-validation) timed the three screens from the
outside -- 2373 ms for a clear, 2386 for Listening, 2375-2446 for the rest,
deterministic to the millisecond -- and every decision since has argued about
that number as one thing. It is not one thing. Between `refresh()` and its
return the panel is reset and re-initialised, two 48000-byte planes go out over
SPI at 10 MHz, a waveform runs, the controller powers its analog side down, and
then two separate `delay(100)` calls run with the image already on the glass.

What started this is the panel looking finished well before the firmware thinks
it is. S5 wrote the same thing down and left it unmeasured -- *"the image is on
the glass before `refresh()` returns"* -- with a window of roughly half a second
guessed at from a button press the walkthrough lost inside a refresh.

**What the library already says, before any measurement.** Reading
`Panel_EPaper::refreshFull()` and `Driver_SSD1677`:

- `Driver_SSD1677::sleep()` is `0x10/0x01` **plus `delay(100)`**, and
  `Panel_EPaper::ePaperSleep()` calls it and then adds **another `delay(100)`**.
  Two hundred milliseconds per refresh, after the image is drawn, unconditional.
  At three refreshes a question that is 0.6 s. The driver's half is ours to
  remove; the panel's half is private and non-virtual, so it can only be
  measured and then argued about upstream.
- Every refresh begins with `wake()` -> `init()`: `hardwareReset(10, 10)`, a
  software reset and the whole register block, before a single byte of image.
- `0x22 = 0xF7` bundles the waveform and the controller's power-down into one
  command, so the library's single busy wait covers both.

**What to measure.** Seven phases per refresh, timed from inside the driver,
for each of the firmware's three shapes -- a full refresh, a partial over the
word band, a partial over the whole panel:

| | |
| --- | --- |
| `cpu` | Panel_EPaper's frame-buffer work: padding, the horizontal-mirror flip of both planes, allocations, the previous-frame memcpy |
| `wake` | `wake()` or `wakePartial()`: reset, software reset, register block |
| `push` | the plane or planes going out over SPI |
| `drive` | `0x22` without its power-down steps, then `0x20`, then BUSY |
| `power` | `0x22 = 0x03`, then `0x20`, then BUSY: disable analog, disable OSC |
| `sleep` | the driver's own `sleep()` and its `delay(100)` |
| `tail` | from that sleep to the return of `refresh()`: the panel's second `delay(100)` |

Plus BUSY traced by interrupt on both edges, which the library's 1 ms poll
cannot do: if the controller drops BUSY between phases, that says more about the
inside of the waveform than anything else here can.

**How.** `src/experiments/e8_refresh_phases.cpp`:

```
~/.platformio/penv/bin/pio run -e exp_e8 -t upload --upload-port <port>
```

Six passes over the three shapes in one boot, then the table and the medians,
then deep sleep with the AI button as the only way back. One boot is one run
because a partial refresh has to follow a full one in the same boot -- which is
the shape of a question anyway.

The rig subclasses the firmware's own `Driver_SSD1677_Sticky` and puts the
timestamps in the overrides, so the library is not touched and the two
corrections in `src/sticky/epaper.h` stay in force. It is the one rig that does
not build the firmware's module for what it measures: `StickyScreen` names its
driver through `Config_Sticky_SSD1677_Fixed`, and the whole point is to put a
different driver under the same panel. It draws the three screens' geometry
itself, with the same faces and the same band. The drawing happens before the
clock starts, so the words on the screen cost nothing and every column is the
panel's own work.

**The split is checked rather than believed.** Taking the last two steps out of
`0x22` -- `0xF7` to `0xF4` full, `0xFF` to `0xFC` partial -- is what separates
the waveform from the power-down. That reading of the bits comes from the
SSD168x family and has not been confirmed against an SSD1677 datasheet, so every
shape is run both ways, alternating, and the rig reports whether the split total
matches the unsplit one. If it does not, the decomposition is wrong and the
`power` column is not a number.

**What this rig deliberately does not measure** is when the pixels become
readable. That is the optical half and it needs a human, a camera or a
photodiode. It was left out because it has no lever attached: the chirp already
precedes every refresh ([S7](e7-upload.md#firmware-upload-validation)), so the
user is told to look up before the panel starts, and the only use for an early
`T_visible` would be aborting a waveform mid-flight -- which costs DC balance
and ghosting. It becomes worth taking only if the `drive` column turns out to
hold a second nobody needs.

**What it unblocks.** [D6](../deferred.md) and [D4](../deferred.md) both weigh a
refresh against a round trip, and [E7](e7-upload.md#e7----what-the-upload-costs) has since
measured the round trip at 0.5 s three times in four. If a third of a refresh is
reset, SPI and two `delay(100)`s, then the display task is not the whole answer:
the panel is a serial resource, three screens a question queue on it, and what
is paid between them is paid on the panel's own timeline where no thread can
help. The numbers say how much of that is removable without touching the
waveform.

## Result

Eighteen refreshes in one boot, six passes over the three shapes, alternating
unsplit and split. The panel is as deterministic as S5 said: every row of a
shape lands within 2 ms of every other, cold first refresh included.

| shape | cpu | wake | push | waveform | power-down | driver sleep | panel tail | total |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| full | 15 ms | 24 ms | 222 ms | 1514 ms | 139 ms | 99 ms | 100 ms | 2115 ms |
| band (136 rows) | 3 ms | 24 ms | 62 ms | 472 ms | 139 ms | 100 ms | 100 ms | 901 ms |
| whole, partial | 12 ms | 23 ms | 222 ms | 472 ms | 139 ms | 99 ms | 100 ms | 1071 ms |

**The split is honest.** Issuing `0x22` twice instead of once cost +1, +1 and
+0 ms against the same refresh in one go, so `0xF4`/`0xFC` plus `0x03` is the
same sequence with a busy wait in the middle, and the power-down column is a
measurement rather than an artefact of the byte.

**338 ms of every refresh runs after the image is drawn.** The waveform ends,
and then: 139 ms of the controller disabling its analog side and its oscillator,
99 ms of `Driver_SSD1677::sleep()`'s `delay(100)`, and 100 ms of
`Panel_EPaper::ePaperSleep()` adding a second one on top. It is the same 338 ms
for all three shapes, because none of it has anything to do with how much of the
panel was refreshed.

**A question spends 4.1 s on the panel, and only 2.5 s of it is waveform.**
Listening plus working plus the answer is 2115 + 901 + 1071 ms. Of that,
**1014 ms is after the image is drawn** and 607 ms is before the waveform
starts. That is the answer to the question this rig was built for: the panel is
finished long before the firmware is, and by a bound that needed no camera.

**BUSY has nothing to say about the inside of the waveform.** Traced by
interrupt on both edges: four edges during the reset and register block, then
one rise when `0x20` lands and one fall 1652 ms later, and nothing in between.
The rise 3 ms after that is the controller acknowledging `0x10/0x01`. So there
is no electrical marker for the phases of the waveform, and the optical half of
the question -- when the pixels become readable -- stays optical. It now has a
bound, though: the image is on the glass no later than 1514 ms into a call that
lasts 2115 ms.

**The planes go out at about a third of the bus.** 96000 bytes in 222 ms is
3.5 Mbit/s against a `SpiBusConfig` of 10 MHz; the band's 27200 bytes in 62 ms
is the same rate, so it is per byte and not per transfer. Nobody has looked at
this: ~145 ms of a full-panel refresh is unexplained overhead in the bus layer,
`src/sticky/epaper.h`'s inversion loop included.

**What S5 measured was the panel plus the drawing.** S5 saw 2373-2446 ms around
`screen.listening()` and this rig sees 2115 ms around `refresh()` alone, having
drawn before it starts the clock. The ~260 ms difference is `fillScreen()` and
`drawString()` -- CPU on the orchestrator's thread, before the panel is touched
at all, and never separated from the panel until now.

## What it changes

- **The display task is not the whole answer, which is what this was for.**
  [D4](../deferred.md)'s task takes the refresh off the orchestrator's thread, and
  the four numbers above are all on the *panel's* timeline, where a thread
  cannot help. Three screens a question queue on one controller, and 1.0 s of
  that queue is the controller finishing work whose result is already visible.
- **Three things are removable without touching the waveform**, in order of how
  easy they are to defend:
  1. **Taken.** `Driver_SSD1677::sleep()`'s `delay(100)`, 99 ms a refresh, is
     now a `sleep()` override in `src/sticky/epaper.h` -- note 3 in that file
     has why nothing waits on it. Re-running this rig against the changed driver
     puts the column at 34-44 *micro*seconds and takes exactly 100 ms off each
     of the three shapes: 2015, 801 and 971 ms, so 3787 ms of panel a question
     instead of 4087. Nothing else moved by more than a millisecond.
  2. The power-down between two refreshes of the same question, 140 ms each, and
     with it the `0x10/0x01` that follows: the controller is shut down and woken
     twice inside one wake for no reason the flow needs, and only the last screen
     of a question has to be left powered down. **Not taken**, and not on time
     grounds -- it needs the driver to be told when a question is over, which is
     a fifth call on an interface `screen.h` keeps four wide on purpose, and it
     leaves the panel's analog side up across a round trip that
     [E7](e7-upload.md#e7----what-the-upload-costs) puts anywhere between 0.5 and 5.5 s.
     What that costs is [E5](e5-sleep-current.md#e5----deep-sleep-idle-current)'s question and E5 is
     not taken. Worth revisiting once [D4](../deferred.md)'s display task lands and
     the panel's own timeline is all that is left.

     *It has landed, at [S10](#firmware-display-validation), and the
     log now shows where this lever would pay.* On a long hold with a quick
     backend the answer queues behind `WORKING` for 37 to 447 ms and reaches
     the glass 2.2 s after the release whatever the round trip was -- the
     screens of one question queued on one controller, exactly as this
     experiment said they would be. Two power-downs and wake-ups between them
     are part of that queue.
  3. `Panel_EPaper::ePaperSleep()`'s own `delay(100)`, 100 ms a refresh. Out of
     reach: it is private and non-virtual, and everything `refreshFull()` touches
     is private too, so there is no subclass that reaches it -- only an upstream
     fix or a panel driven by hand. Worth costing out alongside the bus finding
     above, which lives in the same layer.

  Together that was roughly a second of the four; 300 ms of it is now gone and
  none of it was the waveform.
- **[D6](../deferred.md) and [D4](../deferred.md) still say 2.4 s.** Both weigh a
  refresh against a round trip that [E7](e7-upload.md#e7----what-the-upload-costs) has since
  measured at 0.5 s, and both quote a number that is now known to be a full
  refresh *plus* its drawing. The arithmetic in those deferrals wants redoing
  against what a refresh costs with item 1 taken: 2015 ms, 801 ms and 971 ms.
- **The optical experiment is still not worth taking.** The chirp precedes every
  refresh, so nothing the user waits for depends on when the pixels appear; and
  the only lever an early `T_visible` would offer is aborting a waveform, which
  costs DC balance and ghosting. The 338 ms after the waveform is the part with
  levers on it, and it has now been measured without anyone looking at a screen.

## Firmware display validation

Historical integrated-screen measurements retained from S5/S8/S10/S14. These
include drawing, unlike this experiment's refresh-only phase table. Firmware,
fonts and the redundant sleep delay changed between runs.

S5, before removing the redundant driver delay:

| Screen | Refresh |
| --- | --- |
| `clear()`, cold start only | 2373 ms |
| Listening | 2386 ms |
| the three answers | 2375-2377 ms |
| the three errors | 2443-2446 ms |

Seven full refreshes were visually clean. The error bar added roughly 70 ms
of framebuffer drawing. Button presses intended to mark readability landed
up to 402 ms before `refresh()` returned; they are not a precise optical timing
instrument.

S8, before the display task and delay removal, nineteen questions:

| Transition | Window | Refresh |
| --- | --- | --- |
| `clear()`, cold start only, full | 480 rows | 2386-2389 ms |
| Listening, full | 480 rows | 2398-2403 ms |
| working, **partial** | 136 rows | **990-991 ms** |
| answer, **partial** | 480 rows | **1343-1346 ms** |

Fourteen partials were stable within a few milliseconds. Subtracting the two
partial window sizes gave about 1.03 ms/row and 850 ms fixed cost; comparing
full and whole-panel partial gave about 1056 ms waveform saving. The release
to chirp wait was 2.0–5.6 seconds, with up to 2.1 seconds of unfinished
LISTENING and 990 ms WORKING on the orchestrator's critical path.

S10, display task active and redundant delay removed, before streaming:

| Hold | Round trip | Release to the answer chirp | `WORKING` | Answer queued | Release to the answer on the glass |
| --- | --- | --- | --- | --- | --- |
| 4722 ms, DHCP | 391 ms | 509 ms | drawn | 447 ms | 2200 ms |
| 5352 ms | 658 ms | 777 ms | drawn | 173 ms | 2203 ms |
| 5007 ms | 644 ms | 758 ms | drawn | 191 ms | 2202 ms |
| 5292 ms | 792 ms | 910 ms | drawn | 37 ms | 2204 ms |
| 11922 ms | 1277 ms | 1394 ms | drawn | 0 ms | 2649 ms |
| 2322 ms | 504 ms | 621 ms | drawn, 323 ms late | 668 ms | 2533 ms |
| 2472 ms | 699 ms | 816 ms | drawn, 173 ms late | 302 ms | 2361 ms |
| 957 ms | 484 ms | 598 ms | superseded | 1144 ms | 2986 ms |
| 1137 ms | 205 ms | 318 ms | superseded | 1246 ms | 2808 ms |
| 1137 ms | 221 ms | 335 ms | superseded | 1228 ms | 2807 ms |
| 897 ms | 295 ms | 408 ms | superseded | 1395 ms | 3047 ms |

The panel was checked by eye: no smear and WORKING correctly superseded on
short holds. Release to chirp was round trip plus 113–119 ms; the panel no
longer blocked it. Glass timing still included controller queueing. LISTENING
was 2303–2307 ms, WORKING 891–912 ms, answer 1242–1256 ms; display stack use
was 1.8–2.0 KiB of 8 KiB. Capture elapsed time stayed within -6 to +4 ms of
audio. Streaming later used 1.7–2.2 KiB stack and error partials took
1317–1333 ms; see [E7](e7-upload.md#firmware-streaming-validation).

S13/S14 font and wrapping check, 2026-09-20: four answers of 2, 2, 2 and 3 lines
were readable and correctly centered. WORKING's new 130-pixel band refreshed
in 891–893 ms without smear; wrapped answers took 1259–1273 ms. Display stack
had 5836–5868 of 8192 bytes unused. The longest answer was 151 bytes: ellipsis
overflow was checked in the host preview only, not on the glass.

These observations establish the particular transitions tested, not acceptance
of subsequent dashboard or indicator changes. In particular the controller-RAM
initialization needed after deep sleep is described in the current
[display invariants](../implementation.md#display-invariants).
