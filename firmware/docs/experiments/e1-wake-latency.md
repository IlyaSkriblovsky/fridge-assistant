# E1 -- Wake-to-first-sample latency

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-15; firmware rerun 2026-09-18. Wake to ready chirp: 104.2–104.4 ms, including about 61 ms boot and 41 ms microphone startup. Deep sleep retained. The later timer-wake wrapper change still needs a device rerun.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** Everything between the press and the first usable sample is speech the
device never hears. If it is small, deep sleep stays. If it is large, light sleep
becomes worth its higher idle draw, and the microphone would stay powered.

**What to measure.** From the ext1 wake event to the first sample the capture
path accepts, broken into:

1. ESP32-S3 boot -- image load from flash to the first line of `setup()`.
2. Power latch and mic rail (`StickyMic::begin()` currently waits 10 ms).
3. Microphone settle window (currently 200 ms of discarded audio -- see E3).

**How.** `src/experiments/e1_wake_latency.cpp`:

```
~/.platformio/penv/bin/pio run -e exp_e1 -t upload --upload-port <port>
```

Twenty unattended wakes on the timer, then it waits for the AI button. One row
per wake, then a summary of min/median/max per stage.

**On battery there is no console**, because the console is the USB bridge and USB
feeds the rail -- which hides the one failure the latch code has to be checked
against. So the rig chirps once per wake, after the capture where it costs no
time and records nothing, and keeps every cycle in RTC memory:

1. Flash over USB, unplug it, power the board up with a long press on the AI
   button. The twenty timer cycles then run on their own, one chirp each.
2. Count chirps. Twenty chirps two seconds apart is twenty deep sleep cycles
   survived on battery. Silence means the board switched itself off -- and it
   takes the log with it, since RTC memory does not survive losing power. The
   chirps are the evidence in that case, not the log.
3. **The numbers do not come back afterwards.** Connecting the cable to a
   running board is harmless; *opening the monitor* is what resets it.
   `monitor_rts = 0` and `monitor_dtr = 0` are real, but miniterm applies them
   to a port it has not opened yet and only then calls `open()` -- and opening a
   tty on macOS asserts DTR first. The board's auto-reset circuit takes that
   pulse as a reset, the session restarts and the twenty cycles run again.

   That also settles what the reset does to RTC memory. The monitor is attached
   from the instant the reset happens, so the recovered-session dump would have
   been the first thing on screen. It never appeared, which leaves one
   explanation: the reset arrives through the EN pin, the chip treats it as a
   power-on, and the RTC domain is cleared with everything in it.

   So a battery run inspected afterwards gives chirps and nothing else. Numbers
   would need the log in NVS rather than RTC memory, or a cable with VBUS cut
   and the monitor opened *before* the run -- the reset happens once, at open,
   so the whole run then streams live on battery power. Neither has been needed
   -- see the result below.

 Stages after `setup()`
starts are `esp_timer_get_time()` deltas; the boot stage is not, for the reason
below. Nothing is printed until the capture is over -- at 115200 baud a log line
is several milliseconds injected straight into the path being measured.

**The boot stage needs the RTC counter, not `esp_timer`.** `esp_timer` does not
carry deep sleep: it reads the same small value at the top of every `setup()`
however long the board slept, so it cannot see a boot that ran before it
started, and two readings from either side of a sleep are not on the same scale.

*Corrected 2026-09-16, while checking S2 on the device.* This paragraph used to
say the opposite -- that `esp_timer`'s base is synced to the RTC counter at
startup and therefore includes the sleep. The S2 driver timed the gap between
two wakes both ways and got a negative number from `esp_timer` on all twenty of
them, each equal to minus the time the previous wake had spent awake. The method
below was already on the RTC counter, so neither it nor any number in the result
changes; only the reason was wrong. Nothing else in the tree times anything
across a sleep with `esp_timer`.

What works instead is the timer wake. `esp_sleep_enable_timer_wakeup()` programs
the deadline as the RTC counter at sleep entry plus the requested duration, so
reading `rtc_time_get()` as late as possible before sleeping and again at the top
of `setup()` gives the boot time as a difference against a known deadline. Work
in ticks and convert only that difference: converting the absolute counter would
put the slow-clock calibration error on a value that is hours wide by then.

*Since [S9](#re-run-on-the-finished-firmware-2026-09-18) the deadline is read rather than
worked out.* `esp_deep_sleep_start()` leaves it in `RTC_CNTL_SLP_TIMER0/1`, and
the next boot finds it there. The working-out it replaces was off by up to
10 ms -- the correction under **Watch for** has how.

**Watch for.**

- Measure after a genuine deep-sleep wake. A USB reset takes a different path
  through the bootloader and will give an optimistic number. Every row carries
  `esp_reset_reason()` for that reason -- and a board that dropped its latch and
  switched off instead of sleeping comes back as `POWERON`, which is the same
  symptom as a rig that never woke.
- The boot number comes from the timer wakes, since a button press carries no
  known deadline. That assumes the wake source does not change what the ROM and
  the bootloader do, which holds bar the RTC controller's own trigger path. The
  button phase re-measures every other stage, so the assumption is load-bearing
  for one number only.
- Two systematic biases in the boot number, both sub-millisecond and both
  pessimistic: the gap between the rig's last RTC reading and the deadline
  latched inside `esp_deep_sleep_start()`, and IDF's own deep sleep overhead
  compensation, which shortens the programmed sleep by roughly 750 us.

  *Corrected 2026-09-18, at [S9](#re-run-on-the-finished-firmware-2026-09-18).* Those two
  were not the ones that mattered. A deadline worked out from the counter and
  the previous boot's calibration also differs from IDF's own by however the
  calibrations differ, and taken together that put it anywhere from 10 ms early
  to 5 ms late -- which was the whole of the spread in the result below. The
  deadline does not have to be worked out: it is in `RTC_CNTL_SLP_TIMER0/1`,
  which stays up through the sleep and is not reset by the wake, and both rigs
  now read it at the top of `setup()`.
- There is no bootloader log to read timestamps off. The prebuilt Arduino
  libraries set `CONFIG_BOOTLOADER_LOG_LEVEL_ERROR`, so the `I (NN) boot:` lines
  do not exist in that binary.
- `CONFIG_BOOTLOADER_SKIP_VALIDATE_IN_DEEP_SLEEP=y` is already set there, so the
  wake path skips image validation -- the obvious lever on boot time is pulled
  before the first measurement. The flash runs in DIO (`board_build.flash_mode`)
  while the libraries were built for QIO, which is the lever left if the copied
  segments turn out to dominate.
- The rig's image copies 67 KB of IRAM and 15 KB of DRAM against the firmware's
  77 KB and 23 KB. Flash-resident code is memory-mapped rather than copied and
  validation is skipped, so the rig is not measuring a much smaller boot than the
  real one -- but record both sizes with the result and re-run once the firmware
  is complete.

  *Done at [S9](#re-run-on-the-finished-firmware-2026-09-18), 2026-09-18.* The boot does
  not scale with the image at all, and both sizes are in
  [the re-run](#re-run-on-the-finished-firmware-2026-09-18).
- Expect a split rather than a single number: most of the non-boot dead time is
  delays this project chose (100 ms in the latch, 10 ms for the mic rail, 200 ms
  of settle), and light sleep removes the boot stage, not those.

**Result, 2026-09-15.** Twenty timer wakes and twenty-four button wakes, quiet
room, powered over USB. Milliseconds from the wake event:

| Stage | min | median | max |
| --- | --- | --- | --- |
| Boot -- wake event to the first line of `setup()`, from a worked-out deadline (corrected below) | 51.7 | 56.6 | 60.9 |
| `stickyPower::holdLatch()` | 100.7 | 100.7 | 100.7 |
| Mic rail, USB PHY release, I2S start | 11.2 | 11.3 | 11.3 |
| First block of PCM in the CPU's hands | 15.1 | 15.1 | 15.1 |

**Capture starts 169 ms after the wake event**, when I2S is enabled. The 15.1 ms
after that is one I2S DMA buffer (6 x 240 frames, 15 ms each at 16 kHz) and
costs no audio at all: those samples are already in the buffer, the CPU just
cannot see them sooner. Every row of the run reported `DEEPSLEEP` and `ext1` or
`timer`, so the latch held through the sleep and the numbers are off the real
wake path.

Of the 169 ms, 101 ms is `delay(100)` inside the latch and 11 ms is `delay(10)`
for the mic rail. The boot itself is 57 ms and hardly varies -- the fast-boot
path is already about as short as it goes without a wake stub.

*Corrected 2026-09-18.* It does not vary at all, and it is 62 ms. The spread in
the boot row above is the instrument, not the chip: read from the alarm register
rather than worked out, the same rig boots in 61.8 ms on nineteen wakes out of
nineteen. [The re-run](#re-run-on-the-finished-firmware-2026-09-18) has the
numbers. Nothing decided from this row moves -- the decision had a hundred
milliseconds of margin and this is five.

**Deep sleep stays.** Light sleep would remove the boot and let the microphone
stay powered through the idle, which is worth roughly 100 ms once the two delays
are reconsidered. That is not enough to pay idle current for on a battery
device, particularly when the chirp is what tells the user to start talking.

What the number did change is where to look next: the wake path is delays this
project chose, not silicon.

- The button edge to the wake event is still unmeasured -- it needs a scope, and
  it is expected to be around a millisecond.
- **Acted on:** `delay(100)` in the latch guarded a rail that never dropped --
  the pads are held through the sleep, so on the wake path it waited for
  nothing. `holdLatch()` now takes it only on a cold start.

  **Confirmed on battery, 2026-09-15.** USB unplugged, board powered up with a
  long press, twenty timer cycles: twenty chirps two seconds apart, then silence
  as the rig moved to waiting for the button. Twenty deep sleep wakes with the
  shortened latch, plus the cold start that began the run, and the board never
  cut its own rail. The failure mode is immediate rather than intermittent --
  the rail goes down during the boot that fails to re-assert it -- so twenty
  cycles is not a small sample, it is twenty independent chances to fail at the
  only moment it could.

  The 169 ms breakdown above still stands as the USB measurement from before the
  change. It was not re-measured on battery and does not need to be: the stages
  do not depend on where the power comes from, the only plausible difference is
  a few milliseconds of boot, and the decision it fed has a hundred milliseconds
  of margin.
- Every deep-sleep wake printed `pro cpu reset by JTAG` in the ROM banner, which
  the power-on boot did not. It cost no measurable time, and it is presumably the
  same USB-side mechanism that resets the board when the cable is plugged in.
  Unresolved and not blocking anything.

## Re-run on the finished firmware, 2026-09-18

[S9](#re-run-on-the-finished-firmware-2026-09-18) asked for this, for the reason in
**Watch for**: the boot stage was expected to scale with the image, and the
numbers above are from a rig whose image was smaller than the firmware's.

**The rig's image had caught up on its own.** It compiles every module in `src/`
except `main.cpp`, and since E1 those have come to include WiFi, HTTP and the
panel, so it now copies within a few hundred bytes of what the firmware does:

| Image | IRAM copied on a wake | DRAM copied on a wake | Whole image |
| --- | --- | --- | --- |
| E1's rig, 2026-09-15 | about 67 KB | about 15 KB | -- |
| E1's rig, today | 79588 bytes | 22812 bytes | 1017264 bytes |
| the firmware | 79692 bytes | 23116 bytes | 1148560 bytes |
| the firmware, wrapped | 79692 bytes | 23116 bytes | 1153600 bytes |

Close is still not the same, though. The one part of the image a rig that
replaces `main.cpp` cannot have is `main.cpp` itself, and its globals -- the
panel, the microphone, the WiFi and HTTP objects -- are constructed before
`setup()` runs, inside the very stage being measured.

**So the second rig is the firmware.** `src/experiments/e1_firmware_wake.cpp`
builds the firmware with `main.cpp` in it and has the linker wrap five of its
calls with `--wrap`:

```
~/.platformio/penv/bin/pio run -e exp_e1_firmware -t upload --upload-port <port>
```

| Wrapped | What the rig adds |
| --- | --- |
| `setup()` | the RTC counter and the programmed alarm, before the firmware's first line |
| `stickyPower::holdLatch()` | a timestamp as it returns |
| `StickyMic::begin()` | one on either side |
| `stickyBuzzer::ready()` | one as it starts -- the user is told to talk here |
| `stickyPower::deepSleep()` | the row goes out, and in the timer phase the sleep gets a deadline |

Everything it adds lives in flash, which is mapped rather than copied, or in RTC
memory, which a wake does not reload. The last row of the image table is the
check: the wrapped firmware copies exactly the bytes the firmware does. A timer
wake is the firmware with nobody holding the button, which it already knows how
to handle -- the press is a tap and is discarded, with the association and the
Listening screen on the way as for any press -- so every row is a whole run of
the firmware, from the wake to the sleep. The five names are mangled, since the
linker knows no others, and every wrapper calls through to its `__real_`, so a
function that is renamed or retyped breaks the link rather than quietly going
unmeasured.

**The boot, read from the alarm register:**

| Rig | Deadline | min | median | max | n |
| --- | --- | --- | --- | --- | --- |
| E1's rig, 2026-09-15 | worked out | 51.7 | 56.6 | 60.9 | 20 |
| E1's rig, today | worked out | 55.4 | 60.0 | 64.9 | 19 |
| the firmware, today | worked out | 50.9 | 54.7 | 61.9 | 20 |
| E1's rig, today | **read** | **61.8** | **61.8** | **61.9** | 19 |
| the firmware, today, two runs | **read** | **61.2** | **61.2** | **61.4** | 40 |

- **The boot does not vary, and it does not scale with the image.** Read from
  the register, 61.2 to 61.4 ms on forty wakes of the firmware and 61.8 to 61.9
  on nineteen of the rig. The firmware -- the larger image, with more
  constructors -- is the faster of the two by 0.6 ms, which is unexplained and
  not worth chasing. What E1 was told to watch for is not there.
- **The spread was the instrument.** Each row also carries `skew`, the
  programmed alarm less the worked-out one, which is exactly what the old method
  got wrong: -6.2 to +4.9 ms on the rig (median -2.9), and -9.8 to +0.6 on the
  firmware over two runs (medians -5.9 and -7.0). It wanders by ten
  milliseconds from one wake to the next, and it sits further out on the
  firmware, which spends three seconds on the radio and the panel before each
  sleep, than on the rig, which spends 0.6 s on the microphone. That is the
  shape of a difference in slow-clock calibration rather than of code; exactly
  which calibration IDF sleeps with was not chased, because reading the register
  makes the question moot.
- **It nearly produced a wrong result.** The first run of the wrapped firmware
  still worked the deadline out, and it reported the firmware booting 5 ms
  faster than the rig on the same afternoon. That was the two skews and nothing
  else.

**From the wake to the ready chirp**, the firmware's own path, stage by stage.
Twenty timer wakes a run; the first run is the firmware as S8 left it, the
second is after the change below:

| Stage | What is in it | Before | After |
| --- | --- | --- | --- |
| boot | the wake event to `setup()` | 61.2 | 61.2 |
| latch | `holdLatch()`, with no delay on a wake | 0.3 | 0.3 |
| log | the button, `Serial1`, two log lines, the cached lease | 51.5 | 1.3 |
| mic | the rail's `delay(10)`, the USB PHY, I2S, the 24 ms settle discard | 41.1 | 41.3 |
| task | the PSRAM buffer and the capture task | 0.1 | 0.1 |
| **the wake to the ready chirp** | | **154** | **104.3** |

The before column's boot is the after column's: the first run was the one still
working the deadline out, and the two images differ only in two lines inside
`setup()`, which the boot has finished before. Every stage after `setup()` was the same to 0.2 ms on every wake of
both runs, and so was the total -- 104.2 to 104.4 ms. Five real questions on
the button afterwards came out at 43.0 ms from `setup()` to the chirp on every
one; the log stage was 1.5 ms where there was a lease line to print and 1.3 ms
where there was not.

**Acted on: `delay(50)` after `Serial1.begin()`.** It came with the first demo
in the repository, was explained nowhere, and sat between the latch and the
microphone -- a third of the wake-to-chirp time, in the one stretch of a
question nobody can talk through. E1's rig never saw it because the rig starts
`Serial1` after its capture. It is gone from `main.cpp`, and the first line of
the log arrives intact without it on every wake of the second run and every
question after.

**What is left is 104 ms, and nearly all of it is two things.** 61 ms of boot and
41 ms of microphone -- the rest is 1.7 ms. Two levers on the microphone,
neither taken here:

- **The settle discard costs 30 ms of wall clock for 24 ms of audio.** The DMA
  hands over 15 ms blocks, so a read of 384 samples waits for the second one,
  and it waits on the orchestrator's thread with the chirp behind it. Moving the
  discard into the capture task would let the chirp sound as I2S starts, about
  30 ms earlier. The first 24 ms after the chirp would then not be recorded,
  which no human reaction could reach -- but it changes what the vision's
  step 3 says the chirp means, so it is a decision rather than a fix.
- **The rail's `delay(10)` is a choice, not a measurement.** E3's curve was taken
  with it in place, so taking it out means taking E3 again.

The boot is now a number with no spread at all, which makes it possible to take
apart for the first time. Nobody has.

**Deep sleep stays**, on firmer ground than before. Light sleep would remove the
61 ms of boot, and keeping the microphone powered through the idle would remove
most of the 41 -- about a tenth of a second, still not worth idle current on a
battery device whose chirp already tells the user when to start.

## Timer-wake wrapper compatibility

After startup without AI held was routed to idle, the unattended
`exp_e1_firmware` timer phase stopped reaching the microphone timestamps.
The 2026-09-21 correction wraps only the startup `StickyButton::isDown()` call:
cold/timer wakes enter capture, real button wakes keep their physical state,
and button polling within `button.cpp` stays unwrapped. No production branch
was added. Both environments built and the linked disassembly was checked;
the corrected timer sequence has not yet been rerun on the device. The
2026-09-18 timing result above is not a measurement of this later change.
