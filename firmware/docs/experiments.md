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

| # | Question | Decision it unblocks | Status | Result |
| --- | --- | --- | --- | --- |
| E1 | How long from button press to the first usable audio sample? | Deep sleep vs light sleep | Taken 2026-09-15 | 169 ms to capture start, 57 ms of it boot. Deep sleep stays; latch delay off the wake path |
| E2 | How much does HTTPS add to the round trip versus plain HTTP? | Whether the backend can live in the cloud | Not taken | -- |
| E3 | How long does the microphone actually need to settle? | How much of the first word is lost | Taken 2026-09-15 | Only the first 8 ms is above speech level; discard cut from 200 ms to 24 ms |
| E4 | Does holding the AI button trigger anything in hardware? | Whether push-to-talk can use GPIO4 at all | Taken 2026-09-15, to 20 s | Nothing happens. GPIO4 is usable |
| E5 | What does the board draw asleep? | Whether the button pull-up can keep the RTC domain powered; also [D3](deferred.md) | Not taken | -- |
| E6 | What is the 3.2 s DHCP exchange made of, and what removes it? | Whether the address is cached in RTC memory or fixed, and how long a question waits for the network | Taken 2026-09-17 | 2.1 s waiting for the OFFER, 1.000 s of ARP check. A cached lease gets the device onto the network in 0.18 s instead of 3.3 s |
| E7 | What does the upload cost, and where do the extra seconds in it come from? | The working screen at [S8](implementation.md#s8----the-flow), and [D4](deferred.md) with it | Not taken | -- |

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
- Expect a split rather than a single number: most of the non-boot dead time is
  delays this project chose (100 ms in the latch, 10 ms for the mic rail, 200 ms
  of settle), and light sleep removes the boot stage, not those.

**Result, 2026-09-15.** Twenty timer wakes and twenty-four button wakes, quiet
room, powered over USB. Milliseconds from the wake event:

| Stage | min | median | max |
| --- | --- | --- | --- |
| Boot -- wake event to the first line of `setup()` | 51.7 | 56.6 | 60.9 |
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

It associates through `StickyWifi`, so the association half is the firmware's
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

**A trap found on the way.** `WiFi.persistent(false)` has to be called *before*
`WiFi.mode(WIFI_STA)`. Arduino's default storage is FLASH, so a mode set while
that is still true goes through NVS and costs 1.6 s -- a full half of a cached
connect's entire budget, spent before the association even starts. With the
order right, `WiFi.mode()` takes 33-48 ms. `StickyWifi::begin()` already does it
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

**Watch for.** The penalty has to be separated from the association it follows.
Every question here uploads within a second or two of the address arriving, so
a cost that belongs to a link that has just come up would look like a cost of
the upload. Uploading twice per wake, a few seconds apart, answers that in one
run.

**What it unblocks.** The working screen at S8, and [D6](deferred.md) with it:
2.4 s of partial refresh is worth arguing about against a 300 ms round trip and
is noise against a 7 s one. Also [D4](deferred.md) -- a streaming upload is
worth much more if the cost is per packet than if it is a stall at the front --
and [E2](#e2----https-overhead), which cannot compare HTTPS against HTTP while
HTTP varies by a factor of ten by itself.
