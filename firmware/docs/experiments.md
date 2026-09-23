# Experiments

Measurements this project needs but has not taken. Each one exists because a
decision depends on it and guessing would be worse than waiting.

Fill in **Result** and **Taken** when an experiment is run, and record what the
number changed. A measurement nobody acted on is worth noting too -- "measured,
no change needed" is a result.

Rigs live in `src/experiments/`, one file per experiment, each its own
PlatformIO environment that replaces `main.cpp` and builds the rest of `src/`
unchanged. They are kept rather than reverted: several of these numbers move as
the firmware grows, so they have to be re-measurable in one command.

One rig keeps `main.cpp` instead of replacing it. E1's boot is a property of the
image, and the firmware's image includes `main.cpp`, so E1's second rig is the
firmware itself with five of its calls wrapped at link time -- see
[the re-run](#re-run-on-the-finished-firmware-2026-09-18).

Where an experiment needs something on the host as well, it lives in `tools/`
for the same reason -- an analysis nobody can repeat is not a measurement.

| # | Question | Decision it unblocks | Status | Result |
| --- | --- | --- | --- | --- |
| E1 | How long from button press to the first usable audio sample? | Deep sleep vs light sleep | Taken 2026-09-15, re-run on the firmware 2026-09-18 | The firmware chirps 104 ms after the wake event, 61 ms of it boot, the same to 0.2 ms every time. Deep sleep stays; two delays off the wake path |
| E2 | How much does HTTPS add to the round trip versus plain HTTP? | HTTPS for audio and dashboard | Taken 2026-09-22, repeated 2026-09-23 | Verified HTTPS adds about 0.8 s to a cold connection; strong-WiFi stream tail is 76 ms HTTP vs 80 ms HTTPS; about 53 KiB extra internal RAM |
| E3 | How long does the microphone actually need to settle? | How much of the first word is lost | Taken 2026-09-15 | Only the first 8 ms is above speech level; discard cut from 200 ms to 24 ms |
| E4 | Does holding the AI button trigger anything in hardware? | Whether push-to-talk can use GPIO4 at all | Taken 2026-09-15, to 20 s | Nothing happens. GPIO4 is usable |
| E5 | What does the board draw asleep? | Whether the button pull-up can keep the RTC domain powered; also [D3](deferred.md) | Not taken | -- |
| E6 | What is the 3.2 s DHCP exchange made of, and what removes it? | Whether the address is cached in RTC memory or fixed, and how long a question waits for the network | Taken 2026-09-17 | 2.1 s waiting for the OFFER, 1.000 s of ARP check. A cached lease gets the device onto the network in 0.18 s instead of 3.3 s |
| E7 | What does the upload cost, and where do the extra seconds in it come from? | The working screen at [S8](implementation.md#s8----the-flow) -- settled, a partial refresh -- and [D4](deferred.md) | Taken 2026-09-17 | 128 KB goes up in 660 ms. One ACK in 110 is lost on the way back, and with 5744 bytes in flight there is no later ACK to cover it, so the window stops for a whole retransmission timeout: 1.0-2.5 s, on one upload in four |
| E8 | What is the 2.4 s of a full refresh made of, and how much of it is paid after the image is drawn? | [D6](deferred.md)'s partial refresh and [D4](deferred.md)'s display task, both of which argue against 2.4 s as if it were one number | Taken 2026-09-17 | The waveform is 1514 ms of a 2115 ms full refresh. 338 ms of every refresh runs with the final image already on the glass; 100 ms of that has been taken off in `src/sticky/epaper.h`, leaving a question 3787 ms of panel instead of 4087 |
| E9 | What energy does a dashboard cycle use, including the post-answer wait? | Awake wait vs sleep and reconnect; [D11](deferred.md) | Deferred until dashboard works | -- |
| E10 | Does compressing real dashboard frames improve transfer and energy costs? | Costs of the accepted PNG transport on representative layouts | Deferred until design is chosen | -- |
| E11 | Can retaining the dashboard PNG make LISTENING faster after deep sleep? | Full vs partial LISTENING refresh | Taken 2026-09-23; rejected | 2445–2477 ms total; restoration costs 903–920 ms, mostly bitmap redraw. Full LISTENING restored |

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

*Since [S9](implementation.md#s9----re-run-e1) the deadline is read rather than
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

  *Corrected 2026-09-18, at [S9](implementation.md#s9----re-run-e1).* Those two
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

  *Done at [S9](implementation.md#s9----re-run-e1), 2026-09-18.* The boot does
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

### Re-run on the finished firmware, 2026-09-18

[S9](implementation.md#s9----re-run-e1) asked for this, for the reason in
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

## E2 -- HTTPS overhead

**Why.** The backend is meant to live outside the home network, which means TLS.
On a battery device that wakes for every question, the handshake is paid every
time. The original question was whether this made cloud hosting impractical.
The backend now lives in the cloud, and streaming starts during the hold: the
remaining decision is the cost of HTTPS for short questions and dashboard
fetches, and whether the streaming tail changes.

**What to measure.** Same request over `http://` and `https://`, comparing:

- Time from request start to the first response byte.
- Peak free heap during the request -- TLS buffers are tens of kilobytes and
  compete with the 960 KB audio buffer.

**How.** `src/experiments/e2_https_cost.cpp`, environment `exp_e2`, uses the
production `Backend`, `WifiLink` and `Recording` unchanged. A linker wrapper
around `esp_http_client_init` enables the framework's root certificate bundle
and records `HTTP_EVENT_ON_CONNECTED`. Verification of the hostname, chain and
validity dates stays enabled. SNTP establishes the clock on the first boot,
outside the request timer; RTC time survives the following sleeps.

The hostname comes from `secrets::kBackendBaseUrl` (a hostname without a port or
trailing slash). Both standard ports must serve `POST /audio/fault/500` directly,
with the same device token. No redirects are followed. Only an expected 500
with the complete upload counts as a valid measurement.

```
~/.platformio/penv/bin/pio run -e exp_e2 -t upload --upload-port <port>
~/.platformio/penv/bin/python tools/serial_log.py <port> /tmp/e2.log
python3 tools/e2_summary.py /tmp/e2.log
```

Stop the logger after `E2 DONE`, then restore the `reterminal_e1005` environment.
The rig sends one request per wake, sleeping for one second between requests:
12 adjacent HTTP/HTTPS pairs for each of two payload shapes, 48 requests total.
Order reverses on alternate blocks. All but the labelled initial power-on row
follow deep sleep; no connection or TLS ticket is retained. WiFi power saving
is the production default. The radio, DNS and Internet path are part of the
conditions, so use paired differences and medians, retaining outliers.

- **Tiny:** the 44-byte WAV header only, for the immediate-response comparison.
- **Stream:** 128044 bytes, four seconds of synthetic silence at 512 bytes per
  16 ms, starting after connection. This separates the connection cost from
  the steady-stream response tail. It does not simulate capture overlapping
  the handshake or prove end-to-end button-release latency.

The log's `E2 ROW` columns are cycle, scheme, shape, status, bytes,
DNS/TCP/TLS-to-connected time, open-through-request-headers time,
open-to-response-headers time, end-of-body-to-response-headers time,
synthetic-release-to-response-headers time, longest write, internal heap before,
and minimum internal heap during the request. Times are microseconds; heap is
bytes. The synthetic release is the final sample's nominal deadline, so its
tail includes any outstanding write. `Backend::firstByteUs()` actually marks
**complete response headers**, not the first wire byte; the 500 response body
is not read. There is no chirp or display delay before reading. Heap minima
use the allocator's local minimum monitor, with the production 960 KB recording
allocation already present in PSRAM. Display and capture tasks are not running,
so this measures request allocation cost rather than full-application headroom.

**Watch for.** Session resumption makes the second handshake much cheaper than
the first. Since every question starts from deep sleep, only the cold number
matters unless the session ticket is cached in RTC memory.

**Since [S11](implementation.md#s11----streaming-upload) the handshake is not in
the wait on most questions.** The request opens while the button is held -- at
about 400 ms into the wake on a cached lease -- so the handshake is paid under
the hold, where the user is still talking, and what the scheme can still add to
the wait after the release is the encryption of the last few chunks and of the
answer. The handshake reaches the wait only when the release comes before the
request could open: a question shorter than the network, or one whose stale
lease is being renewed. So the comparison wants three numbers rather than one:
the handshake itself (`Backend::connectedUs()` against `openUs()`), the tail
after the release, and the heap. The first-byte time is the wrong instrument
for now -- the firmware reads the answer after the taken chirp, so a backend
that answers within 60 ms reads as 62 ms whatever the scheme; see
`Backend::firstByteUs()`.


### Result — 2026-09-23 (with the 2026-09-22 weak-signal comparison)

**Taken on the device against the production server**, using the user's supplied
HTTP and HTTPS fault URLs. Both returned 500 directly, without redirects. The
server hostname remains in the ignored secrets file, as required by the vision.
The certificate was verified using the framework CA bundle, not an insecure
TLS mode. Hardware: ESP32-S3 at 240 MHz, USB-powered, Arduino 3.3.7 / IDF library
5.5.0+87912cd291, TLS 1.2 enabled, production WiFi power-save settings. Firmware
base revision: `3444166`; measurement code is the E2 rig stored alongside this
record. One fresh connection per wake; WiFi association is excluded.

The first series completed even though the assistant's turn was interrupted.
The user then restored normal firmware and moved the device closer to the AP.
The second series was freshly flashed and captured on 2026-09-23. Each series
contains all 48 unique cycles, all with status 500 and the expected byte count.
No failed requests were removed. The initial power-on request is retained and
labelled; the remaining 47 requests follow deep sleep.

Raw logs: [2026-09-22](measurements/e2-2026-09-22.log) and
[2026-09-23](measurements/e2-2026-09-23.log). Run `tools/e2_summary.py` on either
file to reproduce the summary. RSSI was -88..-85 dBm (median -86.5) in the first
series, -67..-60 dBm (median -62) in the second. These are two sessions at
different times, not a controlled isolation of RSSI from Internet conditions.

**Good-signal series, 12 samples per cell.** Values below are median
[min..max], in milliseconds. "Response" means complete response headers; this
fault path does not consume the error body.

| Measurement | HTTP | Verified HTTPS |
| --- | --- | --- |
| Tiny request: DNS + TCP (+ TLS), to connected event | 156.9 [131.1..205.3] | 975.3 [920.6..1056.6] |
| Tiny request: open through sent request headers | 159.0 [133.1..207.3] | 977.9 [923.1..1059.2] |
| Tiny request: open to response | 265.2 [235.2..308.7] | 1061.6 [1038.4..1248.3] |
| Four-second stream: open to response, including four-second pacing | 4238.8 [4201.2..4343.5] | 5070.3 [5028.4..5134.6] |
| Stream: last body terminator sent to response | 72.7 [68.6..88.3] | 72.0 [70.0..76.2] |
| Stream: nominal last sample to response, including pending writes | 76.4 [71.8..91.5] | 79.9 [77.9..144.4] |

Adjacent-pair HTTPS-minus-HTTP medians are **800.4 ms** for the tiny request,
**810.6 ms** to the tiny request's connected event, and **3.8 ms** for the
stream's nominal-release tail. The latter's pairwise range is -5.6..72.5 ms.
The difference between medians is not in general the median of differences.
The connected event includes DNS and TCP as well as TLS: these measurements
locate the extra cost in connection setup but do not separate cryptographic
CPU work from TLS network round trips. With 12 samples, the nearest-rank p95
printed by the script is the maximum, not a well-estimated population tail.

**Memory.** Tiny HTTP requests reduced the local minimum of free internal RAM
by a median 4184 bytes; HTTPS by 58272 bytes: **54088 bytes / 52.8 KiB extra**.
The four-second stream uses 4752 vs 58272 bytes, about 52.3 KiB extra. The
lowest remaining internal heap in the good-signal HTTPS series was 206164
bytes. This is rig headroom, not proof of full-firmware headroom with capture,
display and dashboard tasks. The 960 KB PSRAM recording allocation was present.

**The weak-signal series was much less stable during upload.**

| Measurement, median (maximum), ms | Weak HTTP | Weak HTTPS | Good HTTP | Good HTTPS |
| --- | --- | --- | --- | --- |
| Tiny request, open to response | 301 (379) | 1227 (1671) | 265 (309) | 1062 (1248) |
| Stream, nominal last sample to response | 332 (10396) | 454 (3059) | 76 (92) | 80 (144) |
| Longest single write per stream | 76 (2715) | 86 (1406) | 27 (61) | 8 (71) |

Three of twelve weak-signal HTTP streams and one of twelve HTTPS streams had
a single write lasting at least one second. None did in the good-signal series.
That does not establish that TLS improves reliability: there are too few
samples, different packet shapes, and no packet capture. It does show why an
unpaired mean or one slow HTTP upload would misrepresent the TLS cost.

### What it changes

- **The connection overhead is noticeable: about 0.8 s, not a few milliseconds.**
  A short immediate-response request is about four times as long. Moving the
  device nearer the AP removed the observed multi-second upload stalls but
  did not remove the HTTPS connection cost.
- **Steady audio streaming has little additional tail latency in this run.**
  The production capture task continues during connection setup, so a long
  spoken question can overlap TLS with speech. This is an inference from the
  architecture, not a measured complete voice interaction: the rig starts its
  four-second stream after connection and does not exercise the initial audio
  backlog or short holds. Short questions can expose some of the 0.8 s.
- **Dashboard requests cannot hide setup under speech.** Expect an additional
  connection cost of this order for a fresh HTTPS fetch, but the actual 48000-byte
  dashboard download and rendering were not measured by this fault endpoint.
- **A migration looks practical, but “negligible overhead” would be wrong.**
  On 2026-09-23 the user chose to keep HTTP and defer migration under D5.
  E2 is complete; its measurements are retained. A future migration must enable trust and
  reliable time in both Backend and Dashboard, then verify short/long voice
  requests, the audio backlog, dashboard fetches and live memory headroom.
  Initial SNTP took 3891 ms in this run (3421 ms in the earlier run), outside
  request timing; all subsequent wakes retained time and needed no clock wait.
  A cold-boot clock policy therefore matters as well as the measured handshake.
- **This says nothing quantitative about battery life.** USB-powered timing
  and allocator minima are not integrated energy measurements; E9 remains due.

The normal firmware is restored after the experiment. No production transport,
credentials, WiFi power-save setting or server configuration is changed by E2.

## E3 -- Microphone settle window

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

## E4 -- AI button long press

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

## E5 -- Deep sleep idle current

**Why.** The device spends nearly all of its life asleep, so idle current is
what battery life is made of. It is also the one number behind a choice
`stickyPower::prepareDeepSleep()` already makes: the AI button is active low and
its pull-up has to survive the sleep, so the RTC peripheral domain is kept
powered with `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)`.
That was the safe option rather than a measured one. The board may well have an
external pull-up on GPIO4, in which case the domain can go down and the line
comes out.

**What to measure.** Deep sleep current in two configurations:

1. as it stands, with the RTC peripheral domain powered;
2. with that call removed -- checking first that the button still wakes the
   board at all, which is the whole reason the line is there.

Battery life follows from the first number, the wake budget from E1 and however
many questions a day the device is asked.

**How.** No meter, no opening the case, nothing to solder: the board carries its
own instrument. The BQ27220 fuel gauge on the sensor I2C bus sits in the battery
path with its own sense resistor and keeps integrating charge while the ESP32 is
asleep. Standard commands, from the TRM (SLUUBD4A, Table 2-1); all of these are
readable with the gauge sealed:

| Command | Code | Unit |
| --- | --- | --- |
| `Voltage()` | 0x08 | mV |
| `Current()` | 0x0C | mA |
| `RemainingCapacity()` | 0x10 | mAh |
| `AverageCurrent()` | 0x14 | mA |
| `RawCoulombCount()` | 0x22 | mAh |
| `RelativeStateOfCharge()` | 0x2C | % |

**The current registers are the wrong tool.** They report whole milliamps, and a
sleeping ESP32-S3 is expected to draw tens or hundreds of microamps, which reads
as zero. Worse, the gauge can only be read with the host awake, and an awake
host is not the thing being measured.

So the number comes from charge accumulated over a long window instead:
`RemainingCapacity()` or `RawCoulombCount()` at the start, again at the end,
divided by the elapsed time. The device can time that itself -- the RTC counter
runs through deep sleep, so `esp_rtc_get_time_us()` at each wake gives the
interval with no external clock.

Shape of the run: press the button once to take the opening reading, leave the
device asleep for a day or more, press again for the closing one. At 1 mAh
resolution, a day at 100 uA is about 2.4 mAh, so the window has to be long --
and the longer it is, the less the reading wakes themselves matter. Then repeat
with the `esp_sleep_pd_config()` line removed, having first checked the button
still wakes the board at all.

**Watch for.**

- USB feeds the rail, so this only means anything on battery -- and a monitor
  attached afterwards resets the board, so the result has to survive that.
  E-paper holds an image without power and is the natural place to put it; NVS
  is the other. RTC memory will not do, for the reason in E1's battery
  procedure.
- The gauge measures what the battery delivers, so the figure covers the gauge
  itself and anything else always on, not only the ESP32. That is the right
  number for battery life. For the RTC-domain question the two runs are compared
  and whatever is common to both cancels.
- Absolute capacity from a gauge that has never learned this pack can be well
  off. What matters here is the difference between two readings, not either one.
- Nobody has talked to this gauge yet on this unit. Confirming it answers at
  0x55 and returns a plausible voltage is the first step -- and the answer
  screen needs exactly that code anyway, which is why it waits for this
  experiment rather than bringing the bus up on the path where a failure costs
  the user their answer ([D3](deferred.md)).

## E6 -- What the DHCP exchange is made of

**Why.** [S6](implementation.md#s6----wifi) measured a connect on the home
network and found the association is nothing and the address is everything: the
link comes up 77-158 ms after `WiFi.begin()` and the IP arrives 3.14-3.24 s
later, over thirty times as long. Every question pays it between the release and
the upload, so it is the largest single number in the flow that is not the
panel -- and the BSSID cache the vision asks for, which does work, saves about
100 ms of it.

The figure is also too stable to be a busy server: six consecutive wakes came
back within 100 ms of each other. That is the shape of a fixed timer, and a
timer is something firmware can usually stop waiting for.

**What to measure.** Where the 3.15 s actually goes, and what each candidate
removes:

1. The exchange itself -- DISCOVER, OFFER, REQUEST, ACK -- timed from the
   station's side, which needs either a capture on the network or the lwIP DHCP
   client's own state transitions.
2. lwIP's ARP check on the offered address. `CONFIG_LWIP_DHCP_DOES_ARP_CHECK` is
   on by default in ESP-IDF and costs a probe plus a wait before the address is
   handed over. It cannot be turned off from `framework = arduino` without
   moving to `arduino, espidf`, so its share is worth knowing before anyone
   pays that price.
3. The address kept in RTC memory across the sleep and set with `WiFi.config()`
   before `begin()`, which skips the client altogether. This is the cheap
   candidate and the one most likely to work; it needs a fallback to DHCP when
   the lease has gone stale, and a rule for what "stale" means.

**How.** `src/experiments/e6_dhcp_cost.cpp`:

```
~/.platformio/penv/bin/pio run -e exp_e6 -t upload --upload-port <port>
```

It associates through `WifiLink`, so the association half is the firmware's
own code, and alternates wake by wake: an ordinary DHCP wake, then one that
installs the previous wake's lease with `WiFi.config()`. On DHCP wakes the lwIP
client's state machine is sampled every 2 ms through `netif_dhcp_data()`, which
is where the timeline comes from. Every fourth wake installs a lease from a
different subnet on purpose, to measure what a stale one costs.

**An address nobody used is not an address**, so each wake ends with a TCP
connect to the gateway. A refusal proves as much as an acceptance -- something
replied to this address -- and only a timeout means the address does not work.

**Already ruled out.** WiFi power save: `WiFi.setSleep(false)` before the
association changed nothing at all (3.11-3.15 s against 3.14-3.24 s), so the
station is not asleep through an answer that has already arrived. Measured, no
change needed, and the line was taken back out.

**Watch for.** A fixed address is a claim on someone else's network, so a device
that keeps one has to notice when the network disagrees -- another client on the
same address, or a different network entirely behind the same SSID. The safe
version is a cached lease that is tried first and dropped the moment anything
about it fails, which is the same shape as the BSSID cache S6 already has.

### Result

Twelve wakes on the home network, alternating, with every fourth one carrying a
lease from a subnet the device was not on.

| Wake | Address | Probe | Top of setup() to a usable network |
| --- | --- | --- | --- |
| DHCP | 3166-3293 ms | 12-15 ms | 3268-3427 ms |
| cached lease | 39-90 ms | 32-34 ms | 178-226 ms |
| stale lease | 39-44 ms, then 3137-3225 ms of DHCP | 3019-3033 ms of timeouts | 6323-6424 ms |

**The 3.2 s is two things, and only one of them is ours.** The timeline is the
same on every DHCP wake:

```
      44 ms  SELECTING    DISCOVER is out
    2129 ms  REQUESTING   an OFFER came back, REQUEST is out
    2166 ms  CHECKING     the ACK is in, ARP-checking the address
    3166 ms  BOUND        the address is ours
   0 retries, lease 86400 s
```

- **2.1 s of it is the router taking its time over the OFFER**, and it is not a
  lost packet: `tries` is zero on every wake, so nothing was retransmitted. The
  same server then answers the REQUEST with an ACK in 30-50 ms, so it is not a
  slow server in general -- only a slow first answer. Nothing on the device can
  make that faster.
- **1.000 s of it is the ARP check**, to the millisecond, on every wake.
  `CONFIG_LWIP_DHCP_DOES_ARP_CHECK=y` in the prebuilt libraries this project
  links against, and lwipopts.h says the check "lasts 1 - 2 seconds". Turning it
  off means `framework = arduino, espidf` and a menuconfig, which is a large
  price for a third of the number.

**A cached lease removes all of it.** `WiFi.config()` with the previous wake's
address, installed before the association, gets the device onto a network it can
use in 178-226 ms from the top of `setup()` -- against 3268-3427 ms for DHCP,
which is the same connect fifteen times over. The lease this network hands out
is 86400 s, a day, so an address is good for far longer than the RTC memory
holding it survives.

**A stale lease costs one question, not a failure.** An address from the wrong
subnet installs happily and then answers nothing: both probes time out, and
asking properly afterwards takes the usual 3.2 s, for 6.3-6.4 s in total. That
is the whole risk, and it is bounded -- twice a normal connect on the wake that
notices, and back to 180 ms afterwards.

**What it changes.** Caching the lease is worth roughly three seconds of every
question and is the largest single saving available anywhere in the flow. Two
things make it cheaper than it looks in the firmware:

- **The upload is already the probe.** S7 opens a TCP connection to the backend
  on every question, so a cached address that has gone stale shows up as a
  connect that fails -- no extra probe is needed on the happy path, and the
  fallback is to drop the lease, start DHCP and try again.
- **The lease has a known life.** 86400 s from the server, and the RTC counter
  survives deep sleep, so an entry can be aged out rather than trusted forever.

**The firmware now carries it, and the numbers held.**
[S7b](implementation.md#s7b----cached-dhcp-lease) is this result as a step:
3546 ms from the top of `setup()` to a usable network on a DHCP wake against
302-307 ms on a cached one, measured on the firmware rather than on this rig, for
a saving of 3.24 s a question. Two corrections came back with it:

- **The rig's 178-226 ms and the firmware's 302-307 ms are the same number.**
  The difference is the 214-228 ms of microphone, buffer, capture task and ready
  chirp the firmware does before `WiFi.begin()` and the rig did not.
- **A stale lease costs 13.2 s in the firmware, not 6.3 s.** The rule the
  firmware settled on drops the lease on the *second* connect that answers
  nothing rather than the first, so the probe half is paid twice: 10007 ms of
  connects plus 3151 ms of DHCP. The extra 5 s buys not throwing away a working
  address on a connect that failed for its own reasons.

**A trap found on the way.** `WiFi.persistent(false)` has to be called *before*
`WiFi.mode(WIFI_STA)`. Arduino's default storage is FLASH, so a mode set while
that is still true goes through NVS and costs 1.6 s -- a full half of a cached
connect's entire budget, spent before the association even starts. With the
order right, `WiFi.mode()` takes 33-48 ms. `WifiLink::begin()` already does it
in that order; the first version of this rig did not, which is how it was
found.

## E7 -- What the upload costs

**Why.** [S8](implementation.md#s8----the-flow) has to decide what the device
shows between the release and the answer, and the number that decides it is how
long the round trip actually takes.
[S7](implementation.md#s7----upload-and-answer) measured it over nine questions
and the answer was "between 260 ms and 7.4 s", which is not a number a decision
can be taken on.

The spread is not the backend: it answers an empty POST in 4 ms and swallows
120 KB in the same 4 ms, measured over the LAN. It is not the payload either --
two 34 KB recordings took 260 ms and 3319 ms.

**What it looks like.** Against the fastest rate seen, 145 KB/s, four of the
nine questions cost what their size says they should and the other five carry a
penalty of 1.1, 2.4, 2.6, 3.1 and 5.1 seconds. Penalties near whole seconds are
what TCP retransmission timeouts look like -- 1 s, then 2 s, then 3 s -- so the
first hypothesis is that packets are being lost at the front of the connection
rather than that the link is slow. It is a hypothesis and nothing more: nine
samples taken while walking an error table are not a measurement of throughput.

**What to measure.**

- The connect, the body write and the wait for the status line, apart.
  `HTTPClient` reports only their sum, which is why S7's number is one number;
  a bare `NetworkClient` separates them.
- The same upload with `WiFi.setSleep(false)`. [E6](#e6----what-the-dhcp-exchange-is-made-of)
  measured power save against the DHCP exchange and found nothing, but that is
  two round trips. An upload is hundreds of ACKs, which is where power save is
  supposed to cost, and a station that sleeps between beacons is one of the few
  things that would delay a packet by whole seconds.
- The same upload seen from the backend, with `tcpdump`. A retransmit is only
  visible as a retransmit from a machine that can see both copies.
- **The connects that never arrive.** Four of S7's questions could not open a
  connection to a backend that was running and gave up at the 5 s timeout, with
  nothing reaching the backend at all and the wakes on either side answered
  normally. It is very likely the same thing as the penalty above, seen at the
  handshake instead of in the body, and the same capture settles it. It has to
  be settled: [S7b](implementation.md#s7b----cached-dhcp-lease) reads a failed
  connect as a stale lease, and one false reading every fifteen questions would
  have it throwing away an address that was fine.

**How.** `src/experiments/e7_upload_cost.cpp`:

```
~/.platformio/penv/bin/pio run -e exp_e7 -t upload --upload-port <port>
```

Sixteen wakes, four uploads each, all of them the same 128044-byte payload --
`Recording`'s own buffer filled with a tone instead of by the microphone, so
the body is the firmware's allocation and not the rig's invention. Uploads 1 to
3 go through a bare `NetworkClient` at 0, +3 and +6 s after the address arrives,
with the body written in 4 KB chunks so a stall has an offset as well as a
duration. Upload 4 goes through `Backend` unchanged, which anchors the
rig's six clocks to the one number S7 reported. `WiFi.setSleep(false)` on even
wakes.

The capture is taken on the backend, which is the only place both directions are
visible:

```
sudo tcpdump -i en0 -n -s 128 -w /tmp/e7.pcap 'tcp port 8000'
tools/e7_pcap.py /tmp/e7.pcap        # every connection, one line each
tools/e7_pcap.py /tmp/e7.pcap 32     # every packet of connection 32
```

`-s 128` keeps the headers and throws the data away, so the payload length has
to be read out of the IP header rather than counted -- which is the first thing
`tools/e7_pcap.py` gets right and the reason it exists. tshark is not on this
machine and scapy is not in the backend's venv, so the pcap is parsed by hand.

**Watch for.** The penalty has to be separated from the association it follows.
Every question here uploads within a second or two of the address arriving, so
a cost that belongs to a link that has just come up would look like a cost of
the upload. Uploading twice per wake, a few seconds apart, answers that in one
run.

**The rig cannot be watched with `pio device monitor`:** it needs a terminal on
stdin, which an unattended run does not have.
`tools/serial_log.py` reads the port with pyserial instead, and buys something
the monitor does not give -- a host timestamp per line, so the log and the
capture are on one axis:

```
tools/serial_log.py /dev/cu.usbmodem<...> e7-serial.log
```

**What it unblocks.** The working screen at S8, and [D6](deferred.md) with it:
2.4 s of partial refresh is worth arguing about against a 300 ms round trip and
is noise against a 7 s one. Also [D4](deferred.md) -- a streaming upload is
worth much more if the cost is per packet than if it is a stall at the front --
and [E2](#e2----https-overhead), which cannot compare HTTPS against HTTP while
HTTP varies by a factor of ten by itself.

### Result

Sixty-four uploads over sixteen wakes, 7.85 MB up, every one of them answered
200. The phases, over the 48 that went through the bare socket:

| | min | median | max |
| --- | --- | --- | --- |
| connect, upload 1 of a wake | 48 ms | 142 ms | 944 ms |
| connect, uploads 2 and 3 | 7 ms | 15 ms | 1099 ms |
| header | 1 ms | 1 ms | 1 ms |
| body, no stall (35 of 48) | 472 ms | 617 ms | 2742 ms |
| body, with a stall (13 of 48) | 1519 ms | 2067 ms | 5352 ms |
| the backend's own answer | 6 ms | 15 ms | 151 ms |

**128 KB goes up in 660 ms and the backend is not in it.** A clean upload runs
at 194 KB/s and the wait for the status line is 15 ms, which is the LAN figure
S7 already had. Everything S7 could not explain is in the other column.

**The penalty is one chunk out of thirty-two.** A stalled body is a normal body
with a hole in it: take the worst chunk out and what is left is 484/660/2037 ms,
which is the clean column again. The stalls themselves are 1023, 1035, 1047,
1067, 1084, 1291, 1303, 1377, 1472, 1476, 2513, 2553 and 4243 ms -- clustered at
one second and at two and a half, which is what a retransmission timeout and its
first doubling look like.

**Nothing is lost on the way up.** Across 71 connections and some 6700 segments
the backend never saw a hole -- not one segment arrived past the end of what it
already had. What it saw instead was 26 segments arriving *twice*. The device is
retransmitting data the backend has had all along, so the packet that went
missing is the acknowledgement coming back.

**The capture says the ACK was sent and lost, not delayed.** One stall, whole:

```
     213.0  down  ACK 63383     the backend has everything, 0.1 ms after it arrived
    1473.7    up  seq 60511     1261 ms of silence, then the head of the window again
    1473.9  down  ACK 63383     the same ACK, immediately
    1485.4    up  seq 63383     and the upload carries on
```

The backend acknowledged in a tenth of a millisecond and re-acknowledged the
instant it was asked. Nothing on that side was slow. The device simply never
heard it.

**Why one lost ACK costs a whole second: there is only ever 5744 bytes in
flight.** The device sends a burst, waits for the ACK, sends the next -- 2193
bursts across the run, never one byte over 5744. That is not the rig's chunking
and it is not a guess: the `sdkconfig` the prebuilt libraries are built from
says so, and every number in it is on the wire.

| In `esp32s3/sdkconfig` | | On the wire |
| --- | --- | --- |
| `CONFIG_LWIP_TCP_MSS` | 1436 | the segment size in the capture |
| `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` | 5744 | four segments, the ceiling observed |
| `CONFIG_LWIP_TCP_WND_DEFAULT` | 5760 | the window the device advertises |
| `CONFIG_LWIP_TCP_RTO_TIME` | 3000 | the timeout before an RTT has been measured |

Four segments outstanding is the whole of the problem: with one ACK in the air
there is no later cumulative ACK to cover it, so a single loss is a timeout
rather than a hiccup. **The rig's 4 KB chunks are not what produces it** --
upload 4 goes through `HTTPClient`, which writes the body in one call, and its
packets are the same four at a time against a full 5744.

Over the 64 logged uploads:

| | |
| --- | --- |
| bursts sent | 2193 |
| lost ACKs | 20, so one burst in 110 |
| first retransmit after | 999 / 1290 / 2539 ms |
| uploads that paid at least one | 17 of 64 |
| what it costs, averaged over every upload | 471 ms |

An upload is 22 bursts, so a question carries 22 chances at one-in-110 -- which
is why about one upload in four pays and the payload has nothing to do with it.
It is also exactly S7's spread: 238 ms to 6.8 s, unrelated to size.

**Power save is not it.** 9 of 32 uploads retransmitted with power save on, 8 of
32 with `WiFi.setSleep(false)`. The `Backend` numbers came out worse with
it off (median 1085 ms against 532 ms), which is the opposite direction and the
size of the noise. E6 found nothing on two round trips and E7 finds nothing on
two thousand: **measured twice, no change needed.**

**Nor is it the age of the link.** Stalls by position in the wake: 4, 3, 6 and 5
out of 16. Flat. The hypothesis this experiment was told to watch for -- that
the cost belongs to an association that has just finished -- is wrong, and four
uploads a wake is what says so.

What upload 1 *does* pay is the connect: 142 ms median against 15 ms for the
uploads behind it, on every wake. That is a link that has just come up resolving
an address it has not talked to yet. It is 130 ms on the critical path of every
question and it is not worth chasing next to the second above.

**The connects that never arrive did not arrive.** Seventy-one connections, every
SYN answered in 0.1 to 0.7 ms, no SYN ever retransmitted, nothing refused. S7 saw
four failures in about thirty questions and this run saw none in sixty-four,
which at S7's rate is a one-in-twenty-thousand coincidence -- so the two runs
differ in something, and E7 does not settle it.

It does correct the inference, though. S7 read "no file appeared in the backend's
`recordings/`" as *nothing reached the backend at all*. A file only appears when
a POST completes, so that absence proves the upload never finished and says
nothing about whether the SYN arrived. The mechanism found here is a candidate
for the same thing seen at the handshake, and the arithmetic is not comfortable.
A handshake has no RTT sample yet, so its timeout is `CONFIG_LWIP_TCP_RTO_TIME`
flat -- 3000 ms. The connect budget is 5000 ms
([`config::kBackendConnectTimeoutMs`](../src/config.h)). So one lost SYN or
SYN-ACK is survivable with 2 s to spare and a second one is not: the retransmit
after it would be due at 9 s, four seconds past the point where the device has
already given up and shown `NO SERVER`. Unproven, and the way to prove it is a
capture running during a session that reproduces it.

**What it changes.**

- **S8 gets its number.** The round trip is 0.5 s three times out of four and
  1.5 to 5.5 s otherwise. So the working screen is not covering a predictable
  wait -- it is covering a wait that is usually shorter than a full refresh and
  occasionally four times longer. [D6](deferred.md)'s 2.4 s is worth arguing
  about in the first case and irrelevant in the second.
- **[D4](deferred.md) is worth more than it looked.** The cost is not a stall at
  the front: it is proportional to the number of bursts, which is proportional
  to the payload, and it lands anywhere in the body. A streaming upload that
  starts on the press puts the whole body -- stalls included -- under the hold,
  where the user is already waiting. That is the strongest argument the deferral
  has, and it scales the right way for once: a long question has more bursts and
  so more chances to stall, and a long question is the one with room to stream.

  *Taken at [S11](implementation.md#s11----streaming-upload), 2026-09-18, and
  the stalls did more than move.* 2.5 MB went up in fifteen streamed requests
  and no single write took longer than 316 ms. At the rate above -- 17 uploads
  in 64 paying a timeout of a second or more -- that much body would have paid
  about five. The likely reason is the shape of the stream rather than the
  network: a chunk goes out every 10 to 16 ms, so a lost ACK has a later one
  behind it long before a retransmission timeout comes round, which is exactly
  what the 5744-byte bursts above never had. No capture was taken to prove it.
- **[E2](#e2----https-overhead) can be taken now,** but only against the clean
  column. HTTP does not vary by a factor of ten by itself; it varies by a factor
  of ten one time in four, for a reason that has nothing to do with the scheme.
  The comparison has to be made on medians with the stalled uploads identified,
  not on means.
- **[S7b](implementation.md#s7b----cached-dhcp-lease)'s detector is safe on this
  evidence and still unproven.** Nothing in 71 connections would have been
  misread as a stale lease. But the four failures S7 saw are unaccounted for, so
  the rule still wants a second failure behind it.
- **There is a lever, and it is the one E6 found.** A larger
  `CONFIG_LWIP_TCP_SND_BUF_DEFAULT` puts enough in flight that a lost ACK is
  covered by the next one rather than by a timeout -- which is what the same
  upload would already do on a build that had not been trimmed for RAM.

  It cannot be reached from here. `TCP_SND_BUF` is `#define`d to that symbol in
  the port's `lwipopts.h`, and the socket option that would do it at runtime is
  declared `SO_SNDBUF 0x1001 /* Unimplemented: send buffer size */` in the same
  tree. So it is a menuconfig, which means `framework = arduino, espidf` -- the
  price E6 weighed for the ARP check and declined. **That price now buys two
  things rather than one:** a third of E6's DHCP exchange and most of E7's tail.
  Neither is worth it alone; together they are worth costing out before S8
  designs a screen around the tail.

  Nothing about this belongs in the firmware today. It is a build-system change
  with its own risk, and the measurement above is what would justify it.

---

## E8 -- What a refresh is made of

**Why.** [S5](implementation.md#s5----screens) timed the three screens from the
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
precedes every refresh ([S7](implementation.md#s7----upload-and-answer)), so the
user is told to look up before the panel starts, and the only use for an early
`T_visible` would be aborting a waveform mid-flight -- which costs DC balance
and ghosting. It becomes worth taking only if the `drive` column turns out to
hold a second nobody needs.

**What it unblocks.** [D6](deferred.md) and [D4](deferred.md) both weigh a
refresh against a round trip, and [E7](#e7----what-the-upload-costs) has since
measured the round trip at 0.5 s three times in four. If a third of a refresh is
reset, SPI and two `delay(100)`s, then the display task is not the whole answer:
the panel is a serial resource, three screens a question queue on it, and what
is paid between them is paid on the panel's own timeline where no thread can
help. The numbers say how much of that is removable without touching the
waveform.

### Result

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

### What it changes

- **The display task is not the whole answer, which is what this was for.**
  [D4](deferred.md)'s task takes the refresh off the orchestrator's thread, and
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
     [E7](#e7----what-the-upload-costs) puts anywhere between 0.5 and 5.5 s.
     What that costs is [E5](#e5----deep-sleep-idle-current)'s question and E5 is
     not taken. Worth revisiting once [D4](deferred.md)'s display task lands and
     the panel's own timeline is all that is left.

     *It has landed, at [S10](implementation.md#s10----display-task), and the
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
- **[D6](deferred.md) and [D4](deferred.md) still say 2.4 s.** Both weigh a
  refresh against a round trip that [E7](#e7----what-the-upload-costs) has since
  measured at 0.5 s, and both quote a number that is now known to be a full
  refresh *plus* its drawing. The arithmetic in those deferrals wants redoing
  against what a refresh costs with item 1 taken: 2015 ms, 801 ms and 971 ms.
- **The optical experiment is still not worth taking.** The chirp precedes every
  refresh, so nothing the user waits for depends on when the pixels appear; and
  the only lever an early `T_visible` would offer is aborting a waveform, which
  costs DC balance and ghosting. The 338 ms after the waveform is the part with
  levers on it, and it has now been measured without anyone looking at a screen.

## E9 -- Dashboard energy (deferred)

Agreed 2026-09-21; not taken and not required before S15. Measure energy from
battery over a full periodic wake: boot, sensors, association, download, panel
refresh and return to sleep. Include failed-network attempts as a separate case.
Compare the ten-second post-answer wait with WiFi retained against sleeping
for that interval and reconnecting, with otherwise identical dashboard work.
Use the actual transport; if HTTPS is introduced, include its handshake.
Record integrated energy, not just latency or a single current reading, before
changing D11. E5 still owns the board's baseline deep-sleep current.

## E10 -- Dashboard compression (deferred)

Agreed 2026-09-21. On 2026-09-23 the user selected a firmware-only trial of the
server's existing 1bpp PNG. After choosing the layout, save representative
monochrome frames, including sparse and dense content.
Compare the 48000-byte raw baseline with candidate encodings, then measure
download plus decode time, peak memory, firmware size and energy on the device.
Verify exact pixel round trips. Host compression ratios alone do not show an
energy benefit. The user accepted PNG on 2026-09-23; mono1 and format selection are removed.

Firmware trial, 2026-09-23: one cold boot against the unchanged live server
downloaded a 2323-byte PNG (raw baseline 48000 bytes), with 495 ms HTTP request
time and 14 ms decode time on the ESP32-S3. The device completed a 2725 ms full
refresh and entered deep sleep. See the filtered
[UART log](measurements/dashboard-png-2026-09-23.log). This is a functional
smoke test, not a paired latency or energy benchmark; sensor values and network
conditions can change between requests. Full E10 measurements remain deferred.


## E11 -- RTC PNG restoration before LISTENING (rejected)

**Question.** Since the idle dashboard already receives a full refresh, can the
next LISTENING use partial refresh without writing the previous image to flash?

**Trial.** Retain the exact server PNG in one 7168-byte RTC FAST buffer, committing
it only after a successful dashboard refresh. On deep-sleep wake, validate the
cache, decode it into a temporary 48000-byte bitmap, draw that bitmap and the
remembered silent/stale indicators into the panel framebuffer, prime the driver's
previous-image shadow without an optical update, then draw LISTENING and perform
a whole-screen partial refresh. Oversized/invalid caches fall back to full.
No cache writes went to flash. Diagnostics ran on the display task and were
printed by the orchestrator after the voice result, when the panel was idle.

**Evidence.** Four deep-sleep voice wakes on the device, supplied by the user
on 2026-09-23. The retained PNG was 1852–1872 bytes, comfortably within the buffer;
all four restorations succeeded. Exact relevant UART lines are retained in the
[filtered log](measurements/rtc-png-listening-2026-09-23.log).

| Phase | Run 1 | Run 2 | Run 3 | Run 4 |
| --- | ---: | ---: | ---: | ---: |
| PNG bytes | 1859 | 1852 | 1852 | 1872 |
| Validate RTC PNG, ms | 1.714 | 1.615 | 1.648 | 1.576 |
| Allocate bitmap, ms | 0.025 | 0.025 | 0.025 | 0.029 |
| Copy + decode PNG, ms | 16.581 | 16.515 | 16.608 | 16.849 |
| Redraw bitmap + overlays, ms | 751.947 | 745.031 | 760.202 | 748.756 |
| Prime previous-image shadow, ms | 136.967 | 139.921 | 141.745 | 136.022 |
| **Restore total, ms** | **907.274** | **903.147** | **920.267** | **903.271** |
| Prepare LISTENING excluding restore, ms | 321.877 | 325.270 | 322.938 | 323.454 |
| Panel update, ms | 1237.609 | 1216.795 | 1234.126 | 1230.609 |
| **LISTENING total, ms (integer log)** | **2466** | **2445** | **2477** | **2457** |

These are wall-clock durations, including scheduling. Copy/decode includes its
scratch allocations; restoration total includes cleanup and bookkeeping, so it
slightly exceeds the sum of its listed stages. Panel update includes transfers,
waveform and controller sleep, not just the optical waveform. LISTENING total
includes restoration and drawing; it is not a pure partial-waveform measurement.

**Finding.** PNG decoding is not the bottleneck: it takes about 17 ms. Rebuilding
the previous framebuffer through drawBitmap and overlays costs about 0.75 s;
with shadow priming, restoration adds about 0.91 s before the new screen. This
consumes the practical benefit of the shorter partial update. The resulting
2.45–2.48 s is comparable to the historical full-LISTENING cycle (about 2.4 s).
The user reports a clean image but worse perceived responsiveness: the full
refresh exposes readable text partway through its cycle, while this trial spends
substantial time reconstructing the old frame before driving the new image.

**Limits.** This log does not contain a paired full-LISTENING control or an
optical time-to-readable measurement; the perceptual comparison is user feedback.
All four requests subsequently timed out after 30 s, but LISTENING finished
roughly 2.85 s after setup began, before button release or backend timeout. The
log does not establish a causal link between the display change and those
network failures. No energy measurements were taken.

**Decision.** Reject this implementation and restore full refresh for LISTENING.
Remove the retained-PNG cache and its diagnostic code/tests. Keep the server PNG
transport, ordinary partial updates after LISTENING, and full dashboard refresh.
Retain these measurements so a future proposal must account for framebuffer
reconstruction and time to readable text, not only PNG decode or waveform time.
