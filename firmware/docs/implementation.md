# Implementation

The order the firmware gets built in, and the record of what each step turned
out to involve.

The other three documents say what the device is
([project-vision.md](project-vision.md)), what it deliberately does not do yet
([deferred.md](deferred.md)) and what it does not yet know
([experiments.md](experiments.md)). This one says what is being built right now
and in what order -- and afterwards, what each step cost and what it taught.
Notes go in the step they belong to. Anything that changes the design belongs in
the vision, anything that is a shortcut belongs in deferred, and this file links
to it rather than repeating it.

**A step is done when it has run on the device**, not when it compiles. Several
of these can only be checked by looking at the panel or listening to the buzzer,
which means asking.

| # | Step | Delivers | Status |
| --- | --- | --- | --- |
| S1 | Buzzer module | The three patterns, on LEDC | Done |
| S2 | Button module | A press with a length, debounced | Done |
| S3 | Capture into PSRAM | A recording, as a WAV in memory | Done |
| S4 | Capture task | Recording that survives WiFi and the panel | Done |
| S5 | Screens | Listening, answer, error | Done |
| S6 | WiFi | Association without blocking, BSSID cached | Done |
| S7 | Upload and answer | The backend round trip | Done |
| S7b | Cached DHCP lease | Three seconds off every question | Not started |
| S8 | The flow | Wake, record, ask, show, sleep | Not started |
| S9 | Re-run E1 | Wake latency of the real firmware | Not started |

## Why this order

Bottom-up, so that every step can be checked on the device by itself, and the
pipeline is assembled last out of parts that are already known to work. The
alternative -- wiring the whole flow early and filling it in -- means the first
failure is somewhere in a chain of seven things, on a device whose only console
disappears the moment it runs on battery.

The buzzer comes first because it is the only feedback that works on battery,
and every later step's error path wants it.

---

## S1 -- Buzzer

`src/sticky_buzzer.h/.cpp`. Three patterns, from the vision's table: ready (two
very short notes, low then high), answer (two short notes, high then low), error
(one longer note).

- **LEDC, not `tone()`** -- and blocking, because a chirp issued just before
  `esp_deep_sleep_start()` has to finish before the pads are parked. The E1 rig
  has a one-note version of exactly this (`chirp()`, 3 kHz for 60 ms); the
  module generalises it.
- **Leave the pad low afterwards**, as the rig does, so the parking in
  `prepareDeepSleep()` holds it there.
- Frequencies and durations are a choice, not a measurement. Pick them, listen
  to them, and record what was picked here -- the requirement is only that the
  three are distinguishable without looking.

**Verified by** ear.

### What was picked

| Pattern | Notes | Total |
| --- | --- | --- |
| ready | 2000 Hz 40 ms, rest 30 ms, 3000 Hz 40 ms | 110 ms |
| answer | 3000 Hz 90 ms, rest 50 ms, 2000 Hz 90 ms | 230 ms |
| error | 1500 Hz 450 ms | 450 ms |

The two pairs share the same two notes and differ only in direction, so the
normal outcomes are opposites; the error note is below both and long enough that
it cannot be heard as a pair that was cut short. Kept near 2-3 kHz because that
is where a small piezo is loudest -- 1500 Hz was the one value expected to come
out quiet, and did not. Confirmed on the device: the three are distinguishable
without looking.

`play()` takes a `Note{hz, ms}` array with `hz == 0` as a rest, so the patterns
are three static arrays and re-tuning one costs a line. LEDC is attached once per
pattern rather than once per note, or every gap would carry a pad glitch.

The listening test ran from a temporary driver in `main.cpp`, removed with this
step: three passes and then silence, rather than a repeat from `loop()`. **The
board has no off switch**, so a demo that repeats can only be stopped by pulling
the battery, while reset replays a fixed number of passes on demand. Worth
knowing at [S5](#s5----screens), which also needs a temporary driver.

## S2 -- Button

`src/sticky_button.h/.cpp`. The AI button on GPIO4, active low, internal
pull-up.

- `rtc_gpio_deinit()` first: GPIO4 comes out of the sleep as an RTC pad and
  `digitalRead()` means nothing until the GPIO matrix has it back.
- The press that woke the board **is still held** when `setup()` runs, so the
  firmware reads a level and times it, rather than waiting for an edge.
- Release is debounced: `config::kButtonDebounceMs` (40 ms) of stable high
  before the press counts as over.
- A press shorter than `config::kButtonMinHoldMs` (300 ms) is a tap and gets
  discarded, silently -- including the case where the ready chirp has already
  sounded, which it will have ([D7](deferred.md)).
- The side buttons (GPIO5/6) stay unused.

**Verified by** the hold length over Serial1 across a few dozen presses, short
and long, with attention to whether a release ever registers twice.

### What it turned out to involve

`StickyButton` is `begin(pressStartUs)`, `poll()`, `heldMs()`, `isTap()` and
`isDown()`. `begin()` takes the start of the press as an argument rather than
reading the clock, because the press is older than the firmware: the caller
passes the earliest timestamp it has, which is the top of `setup()`. `poll()`
never blocks and latches the release, so a late bounce cannot end the same press
twice -- and from [S4](#s4----capture-task) it is the capture task that calls
it, between I2S reads.

**Two floors under the measurement**, neither of which touches the tap/question
decision, both worth knowing before anyone reads a hold as exact:

- **Every hold reads about 60 ms short.** The press starts before the boot does,
  and a button wake carries no deadline to measure the boot against
  ([E1](experiments.md)). So `kButtonMinHoldMs` of 300 ms asks for roughly
  360 ms of real press.
- **A press shorter than the ready chirp reads as the chirp's length.** The
  chirp blocks for 110 ms and nothing polls during it, so short taps came back
  as exactly 161-162 ms, over and over. It disappears on its own at S4, when the
  poll moves into the capture task, which does not stop for a chirp.

The driver was one press per wake -- sleep, press, one row, sleep -- which is
the real path rather than an imitation of it: the press is already down when
`setup()` runs, and only a wake reproduces that. It replaced the display/WiFi/
HTTP bring-up demo in `main.cpp`, which could not coexist with it; those pieces
come back as real modules in S5 to S7.

**The bounce question needed an instrument.** A press that produces two rows is
the failure to look for, but a bounce re-waking the board looks exactly like a
quick tap in the log -- same wake cause, same tiny hold. The rows had nothing to
tell them apart until the driver started reporting how long it had slept since
the previous row: a bounce comes back about a boot later, ~60 ms, while a human
pressing again is hundreds of milliseconds at the very least.

That is also where a claim in [E1](experiments.md) turned out to be wrong.
Timing the gap with `esp_timer` gave a negative number on every wake, each equal
to minus the previous wake's awake time -- `esp_timer` restarts on every deep
sleep wake and does not carry the sleep, whatever E1 said. The measurement is on
the RTC counter, and E1 has been corrected.

**79 presses over three flashes**, and on the last 20 the gap between wakes was
measured: the smallest was 196 ms, three times the bounce floor, and no press
ever produced two rows. The threshold lands where `config.h` puts it -- 303 and
307 ms came back as questions, 272 and 282 ms as taps -- and a 14 s hold rode
through with a line a second, which re-confirms [E4](experiments.md) on the
firmware's own code rather than on a rig.

## S3 -- Capture into PSRAM

Two pieces:

- `StickyMic` gets a raw-sample read alongside `readLevel()` -- one I2S read,
  no averaging, straight into a caller's buffer. `readLevel()` stays: the E1/E3
  rig uses it and levels are still worth logging.
- `src/sticky_audio.h/.cpp` owns the buffer: one `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
  of 44 bytes + `kMaxRecordSeconds` x 32 KB, which is 960 KB of audio. **The
  44-byte WAV header is reserved at the front** and filled in once the length is
  known, so sending never copies a megabyte to prepend it.

I2S reads land directly at the write offset inside the PSRAM buffer, so there is
no intermediate chunk and no second copy.

A failed allocation is `NO MEMORY`, now in the vision's error table. It is the
one failure where nothing works at all, so it is worth a screen of its own
rather than being folded into `NO MICROPHONE`.

The samples carry the microphone's DC bias -- the S3's PDM-to-PCM path has no
high-pass stage. For now it goes to the backend as it is; removing it here would
be a second pass over a megabyte for something the backend can do while it
decodes. E3's open question (whether a high-pass would shrink the settle window
to nearly nothing) is the same subject and is tracked there.

**Verified by** recording a fixed few seconds, then logging block levels across
the captured buffer -- speech has to show up where it was spoken. A dump of the
buffer to the backend, once S7 exists, is what actually proves it is audio.

### What it turned out to involve

`StickyAudio` is `begin(sampleRate)`, three lines per chunk, and `wav()`:

```
const uint32_t want = audio.nextChunkSamples();          // 0 at the cap
const uint32_t got = mic.readSamples(audio.writeHead(), want);
audio.commit(got);
```

`writeHead()` hands out an address inside the PSRAM allocation, so samples go
from the DMA to their final place in one step and nothing is copied afterwards.
`wav()` writes the 44 reserved bytes at the front and returns the buffer, which
from [S7](#s7----upload-and-answer) on is the POST body unchanged.

`StickyMic::readSamples()` is the raw read that fills it: one `readBytes()`, no
averaging and no DC removal. Adding it pulled the level arithmetic out of
`readLevel()` into `MicLevelMeter`, because the dump needs exactly the numbers a
live reading gives, over a slice of PSRAM instead of over an I2S read.
`readLevel()` now feeds the meter chunk by chunk and is otherwise unchanged.

**The ready chirp cannot be inside the recording while there is only one task.**
`stickyBuzzer::ready()` blocks for 110 ms and the I2S DMA holds 90 ms, so a
chirp between two reads overflows the ring and tears a hole in the recording.
The driver chirps *before* `StickyMic::begin()` instead, which costs the user
the 34 ms of rail and settle that follow and leaves the timeline in the dump
exact -- which is the thing this step is checked on. Nothing about the flow
changes: this is the vision's concurrency argument arriving one step early. But
it does settle a question [S8](#s8----the-flow) would otherwise have to ask
again -- **the chirp belongs to the orchestrator, not to the capture task**, and
it is [S4](#s4----capture-task) that makes the vision's order free.

### What it measured

Two recordings, one from a reset into a quiet room and one from a button wake
with two words, a pause and two words.

- **The buffer is where it should be.** 960044 bytes at `0x3c050f18`, which
  `esp_ptr_external_ram()` confirms is PSRAM, leaving 7418128 of 8388608 bytes
  free. 44 bytes of header and 960000 of audio, which is the 30 s cap exactly.
- **Nothing was dropped.** 80000 samples for a 5000 ms recording, twice, off
  chunks of 256 -- no short read anywhere, so `readBytes()` really does return
  everything asked for as long as the caller comes back inside the 90 ms.
- **The header is right**, byte for byte: `RIFF` 160036, `WAVE`, `fmt ` 16, PCM,
  1 channel, 16000 Hz, 32000 B/s, block align 2, 16 bits, `data` 160000. Checked
  by hand because until S7 exists nothing else would notice a swapped field.

The spoken run, in 100 ms blocks:

| Blocks | At | Level | What |
| --- | --- | --- | --- |
| 1-3 | 0-300 ms | -37.7 -> -57.0 dBFS | the power-up transient, still decaying |
| 4-7 | 300-800 ms | about -67 dBFS | room |
| 14-20 | 1300-2000 ms | -50 to -44 dBFS | the first two words |
| 21-32 | 2000-3200 ms | about -69 dBFS | the pause |
| 33-41 | 3200-4100 ms | -61 to -43 dBFS | the second two words |
| 42-50 | 4100-5000 ms | about -69 dBFS | room |

Speech where speech was, silence where silence was, and both at the levels the
vision records for this unit -- a quiet room near -70 dBFS, speech at arm's
length peaking near -45. The first three blocks are [E3](experiments.md) seen
again at a coarser grain: the transient is below speech level from about 200 ms
and at the floor by 400 ms, measured this time on the firmware's own code rather
than on a rig. It is also the reason the driver's own dump opens loud, and why
the loudest block of a recording is the first one -- worth remembering before
anyone reads that as a fault.

## S4 -- Capture task

The capture task, pinned to core 1 above `loopTask`'s priority; the WiFi and
lwIP tasks sit on core 0. It starts as soon as the microphone is up, reads I2S
into the buffer, and stops on release, on the 30 s cap, or on an abort from the
orchestrator (a WiFi failure during recording).

The I2S DMA holds 6 x 240 frames -- 90 ms at 16 kHz -- which is the whole budget
this task has to stay inside. A blocking WiFi connect or a panel refresh would
eat it several times over, which is why they are not in it.

**The button poll goes in this task**, between I2S reads. That is a refinement
of the vision's "does nothing but read I2S": a `gpio_get_level()` costs nothing
and never blocks, and it buys a tight stop. The orchestrator polls in a loop
that also refreshes the panel, so a release arriving during a full refresh would
otherwise add a second or two of room noise to the end of every recording. It
held up on the device and has moved into the vision's concurrency decision.

**Verified by** recording while WiFi associates and the Listening screen
refreshes, then checking the buffer for gaps -- this is the step the two-task
split exists for, so a clean recording under both loads is the result.

### What it turned out to involve

`src/sticky_capture.h/.cpp`. `StickyCapture` is `start()`, `abort()`,
`finished()`, `wait()` and `stop()`, and the orchestrator's half of it is five
lines:

```
if (!mic.begin()) fail("microphone", mic.lastError());
capture.start(mic, audio, button);
stickyBuzzer::ready();      // on this task, while the recording already runs
...                         // WiFi, the Listening screen, seconds of both
capture.wait();
```

**Ownership is handed over, not shared**, which is why there is no locking
anywhere in the three modules the task drives. Between `start()` and the moment
`finished()` first returns true, the microphone, the buffer and the button
belong to the task and the orchestrator must not touch them. Exactly two things
cross the boundary while it runs: the abort request going in, a flag the task
notices between two reads, and the finish coming out, a binary semaphore whose
give carries the barrier that makes everything the task wrote visible to
whoever comes back out of `wait()`.

**Core 1 at priority 10.** `loopTask` runs at 1, so a DMA buffer coming ready
preempts the orchestrator rather than queueing behind a panel refresh; the WiFi
(23) and lwIP (18) tasks are pinned to core 0, so the recording and the network
never compete for the same core at all. The task prints nothing -- Serial1
belongs to the orchestrator, and a line at 115200 is a millisecond the reads do
not have to spare.

Four ways to stop, and the orchestrator needs all four apart: a debounced
release, the 30 s cap, an abort, and a microphone that stopped delivering. The
cap is a question like any other; the abort is the vision's rule that a
recording with nowhere to go is ended rather than finished, and the driver
exercises it by aborting when the association times out.

**The ready chirp is back where the vision puts it.** S3's driver had to chirp
before `StickyMic::begin()`, because 110 ms of blocking between two reads of a
single-task loop tore a hole in the recording. Now it sounds on the orchestrator
while the capture task keeps reading, and it is audible in the first block of
every dump with nothing missing behind it -- which is the question
[S3](#s3----capture-into-psram) left open, answered on the device.

**The floor under a short press is gone**, as [S2](#s2----button) predicted. The
button is polled every chunk -- 256 samples, 16 ms -- from the moment the task
starts, so a press no longer reads as the length of whatever the orchestrator
happened to be blocked in.

The driver is the real path again, minus the upload: press, chirp, speak through
a full refresh and an association, release. WiFi and the panel are raw in
`main.cpp` rather than modules, since [S5](#s5----screens) and
[S6](#s6----wifi) are what give them one; all this step needs of them is an
orchestrator that is genuinely busy for seconds on end.

### What it measured

Three recordings under both loads, the full refresh (2.60 s, every time)
covering the first two thirds of each and the association (0.68-1.27 s) inside
it.

| Held | Recorded | The task's own clock | Missing |
| --- | --- | --- | --- |
| 3832 ms | 3760 ms, 235 chunks | 3764 ms | 4 ms |
| 5032 ms | 4960 ms, 310 chunks | 4964 ms | 4 ms |
| 4342 ms | 4272 ms, 267 chunks | 4274 ms | 2 ms |

- **Nothing was dropped.** Time that passes without samples to show for it is
  audio the DMA threw away, and across three runs it came to 2-4 ms -- a
  fraction of one chunk, which is the read still in flight when the release was
  seen. Each dump has the speech where it was spoken at the usual -45 dBFS, the
  first burst falling entirely inside the panel refresh, and no discontinuity
  anywhere in it.
- **The loads cost nothing measurable.** The longest gap between two reads was
  30.1 ms of the 90 ms the DMA holds -- and none of it is the refresh. 16, 21
  and 18 reads took half again as long as a chunk, against 15.7, 20.7 and 17.8
  predicted by arithmetic alone: the DMA delivers 240-frame blocks and the task
  asks for 256 samples, so every fifteenth read waits for a second block. The
  first one is always chunk 7, once the head start left by the settle read has
  been eaten. Every slow read in the three runs is the DMA's own granularity and
  not one of them is the orchestrator.
- **That gap is not the headroom**, and it should not be read as a third of the
  budget spent. A read that waits is a read blocked *inside* `readSamples()`,
  which is the safe place to be; what costs audio is time spent away from the
  read, and the only column that measures that is the missing milliseconds.
- The 227 ms hold at the top of every log is the cold boot -- a flash, or the
  reset that opening the serial monitor causes, has no button down, so the press
  ends on the debounce window with 64 ms of power-up transient recorded. It is
  the driver's way of saying "press the button".

## S5 -- Screens

`src/sticky_screen.h/.cpp`: `listening()`, `answer(text)`, `error(title, detail)`.
A small interface on purpose -- D4 moves the display into its own task later,
and that is cheap only if nothing else calls the panel directly.

- Full refresh on all three transitions ([D6](deferred.md)).
- **No `setRotation()`.** The library's default is already the orientation the
  vision's screen section settles on -- buttons along the bottom, on the right,
  under the right thumb. Both ways round were put on the panel to choose it, so
  a rotation appearing here later is a regression rather than a fix.
- Only `LOAD_GLCD` and `LOAD_GFXFF` are compiled into Seeed_GFX2, so the answer
  is a FreeFont through `setFreeFont()`. Font 1 at 8x8 is unreadable on a 4"
  panel without scaling.
- ASCII only ([D1](deferred.md)). A non-ASCII byte should degrade visibly but
  not draw garbage.
- No battery level ([D3](deferred.md)).
- Error strings come from the vision's table: `NO WIFI`, `NO SERVER`,
  `SERVER ERROR` plus status, `BAD RESPONSE`, `TIMED OUT`, `NO MICROPHONE`,
  `NO MEMORY`.
- **No word wrap** ([D2](deferred.md)). A long answer runs off the right edge
  and that is accepted: the backend returns a fixed phrase, so there is nothing
  to wrap until there is a model behind it.

**Verified by** the user looking at the panel. Worth driving all three screens
from a temporary `loop()` before the flow exists, since this is the one step
where the failure mode is cosmetic and only a human can see it.

### What it turned out to involve

`StickyScreen` is `begin()`, `clear()` and the three screens, and it is the only
place `Seeed_GFX` is touched -- `sticky_epaper.h` is now included by
`sticky_screen.cpp` rather than by `main.cpp`, so the rest of the firmware never
has to know the glass is wrong twice. `begin()` failing is the vision's "nothing
to draw on": it reports the library's own message through `lastError()` and the
caller chirps, logs and sleeps.

**Nothing draws by itself.** `begin()` only brings the panel up, and the
pre-clear [S8](#s8----the-flow) asks about is `clear()`, a call of its own. That
keeps the cold-start question where it belongs -- the driver already answers it
with `stickyPower::wokeFromDeepSleep()`, which is exactly what S8 will do.

**Three faces, at 235 dpi.** `FreeSansBold24pt7b` doubled is the Listening
screen: 516 px of `LISTENING` on an 800 px panel, by a long way the largest of
the three because it is the one screen read from across a room rather than in
the hand. The same face at size 1 is an error title, white inside the bar --
the widest in the vision's table is `NO MICROPHONE` at 416 px, so every title
clears the bar's edges. `FreeSans24pt7b` is the answer and `FreeSans18pt7b` the
error detail. Together they cost about 22 KB of flash, which is nothing against
the 3 MB partition.

**A black bar is what tells an error from an answer** before either has been
read. The alternative -- three screens of black text on white, differing only in
wording -- makes the user read the screen to find out whether anything went
wrong, on a device whose whole point is that it is glanced at.

**Non-ASCII degrades per character, not per byte.** `screenDrawableText()` keeps
0x20-0x7E, skips UTF-8 continuation bytes and writes one `?` for everything
else, so a Russian word that slips past the backend's transliteration
([D1](deferred.md)) comes out as one `?` per letter instead of two or three. It
is the only part of the module that can be reasoned about away from the device,
which is why it is a free function rather than a private method. Sanitising also
keeps `textWidth()` honest: it walks bytes while `drawString()` decodes UTF-8,
so the two disagree about the width of anything multi-byte, and every datum but
`TL_DATUM` is computed from that width.

The driver is a walkthrough -- one screen per press of the AI button, seven of
them, covering the Listening screen, the phrase the backend really returns, an
answer too long for the line, an answer with Cyrillic in it, an error title on
its own, one with a detail, and the widest title in the table. Seven full
refreshes back to back is also a ghosting test the flow itself never performs.
**The board has no off switch**, so, like S1's listening test, nothing repeats:
the walkthrough ends in deep sleep and five idle minutes end it early. One
minute was tried first and was too short to walk away from the desk mid-screen.

**The image is on the glass before `refresh()` returns**, and the first version
of the driver lost every press made in between. It polled the button after the
refresh, so a press that started and ended while the panel still had the task
had simply never happened -- which is exactly what someone does when they press
as soon as the screen appears. The driver now latches the falling edge from an
interrupt and compares against a snapshot taken *before* the refresh.

The flow does not have this gap and never did: from [S4](#s4----capture-task)
the capture task polls the button every 16 ms right through the Listening
refresh, which is the reason the two tasks are split. It is worth knowing anyway
because it puts a number on "the panel is behind everything" -- there is a
window of roughly half a second where the user has read the screen and the
firmware is still inside the library.

### What it measured

The panel's full refresh is deterministic to the millisecond -- the duration is
the controller's waveform table, not our code, and a cold start repeated the
same two numbers exactly.

| Screen | Refresh |
| --- | --- |
| `clear()`, cold start only | 2373 ms |
| Listening | 2386 ms |
| the three answers | 2375-2377 ms |
| the three errors | 2443-2446 ms |

- **The pre-clear costs a screen's worth of time**, which is what S8's note is
  about: 2373 ms on a `POWERON`, and not spent at all on an `ext1` wake, where
  the first log line after the wake is already the Listening screen.
- **The black bar costs about 70 ms**, all of it in `fillRect` filling the frame
  buffer. The SPI transfer and the waveform are the same either way.
- No ghosting anywhere in seven consecutive full refreshes, including the three
  that follow a screen with a band of black across it. `LISTENING` reads from
  across the room, `NO MICROPHONE` sits inside the bar, and the orientation is
  the one the vision settles on -- checked on the panel.

**Where the 2.4 seconds goes.** About 220 ms of it is fixed cost the library
adds and 2.2 s is the panel's own waveform, waited out on the BUSY pin:

- `Driver_SSD1677::sleep()` ends with `delay(100)`, and
  `Panel_EPaper::ePaperSleep()` adds another 100 ms on top of it -- 200 ms of
  unconditional delay after the physical refresh has already finished.
- The library puts the controller into deep sleep after every refresh, so the
  next one begins with `hardwareReset(10, 10)`, a software reset and a fresh
  upload of the waveform table: 21 ms of delay plus the transfer.

Shortening the remaining 2.2 s means a different waveform, which is what partial
refresh and `refreshFast()` are -- [D6](deferred.md), and not before the UI/UX
pass.

**The screen is readable about 0.4 s before the firmware knows it.** Six presses
made as soon as each screen appeared landed at -295, -402, -21, -224 and -70 ms
relative to the moment `refresh()` returned, and one deliberate slow press at
+194 s. Five of the six were made while the panel still had the task -- every
one of them was caught by the latch, and every one would have been lost by the
poll it replaced. The widest, 402 ms, is twice the 200 ms of library delay, so
most of that window is the tail of the waveform rather than the delays.

## S6 -- WiFi

Association without blocking: `WiFi.begin()` and then poll `WiFi.status()` in
the orchestrator, never `waitForConnectResult()`. The IDF tasks make progress on
their own; what must not happen is the orchestrator sitting inside a call while
the button is released.

- **BSSID and channel cached in RTC memory** across sleeps and passed to
  `WiFi.begin(ssid, pass, channel, bssid)`. On failure, fall back to a plain
  `begin()` and re-cache whatever worked. Reconnect time is what the 30 s buffer
  is covering, so this is not an optimisation to leave for later.
- Needs a new constant in `config.h`: how long association gets before the
  attempt becomes `NO WIFI`. Nothing in the vision fixes that number.
- A drop during recording aborts immediately rather than letting the user finish
  talking into a recording with nowhere to go.

**Verified by** connect times over Serial1, cold and cached, including the first
boot after the cache was written and a run with the access point moved to
another channel.

### What it turned out to involve

`src/sticky_wifi.h/.cpp`. `StickyWifi` is `begin()`, `poll()` and `end()`, and
the orchestrator's half of it is three lines:

```
wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword);
...                                     // the Listening screen, the recording
if (wifi.poll() != StickyWifi::State::Online) capture.abort();
```

**The class is a clock, not a state machine over `WiFi.status()`.** The attempt
is a series of windows inside one budget: the cached AP gets
`config::kWifiCachedAttemptMs`, and after that each fresh scan gets 6 s, until
`config::kWifiConnectTimeoutMs` ends the question with `NO WIFI`. With the
numbers as they stand that is 3 + 6 + 6, which is the timeout exactly.

**A window only runs while there is no link**, which is what the first run on
the device taught. A window is for an association that is not happening; once
the link is up the attempt has done the hard part and is waiting for DHCP, which
on this network is over three seconds and is not made faster by throwing the
association away and starting again. The first build did throw it away: every
cached connect came back as `fallback, 2 attempts` and took 6.3 s instead of
3.9 s, because the cache's 3 s window expired in the middle of a DHCP exchange
that was going perfectly well. Past the link the budget is the only thing left
that can end an attempt.

Deciding a retry on the status instead is the obvious design and it is wrong
here, for two reasons that only show up on the device:

- **The status is history until the next event.** `WL_NO_SSID_AVAIL` and
  `WL_CONNECT_FAILED` survive into the following attempt and are only overwritten
  when that attempt produces an event of its own, so a healthy association would
  be torn down on the strength of the previous one's result. `WL_DISCONNECTED`
  cannot stand in for "failed" either -- it is also what the status reads for
  most of a connect that is going to work.
- **The library retries the first failure by itself**, whatever
  `setAutoReconnect(false)` was told: WiFiSTA keeps a `first_connect` static and
  reconnects once on the first disconnect event, reusing the config it was given
  -- cached BSSID included. So a stale cache is retried with the same stale
  cache, and the only thing that can end it is a clock.

A window several times longer than a connect that works cannot cut a good
attempt short, which is what makes the arithmetic above the whole mechanism.

**`WL_CONNECTED` means an IP, not an association.** The status reaches
`WL_IDLE_STATUS` when the link comes up and `WL_CONNECTED` only on DHCP's reply,
so `linkMs()` and `elapsedMs()` report the two halves apart: the gap between
them is DHCP and nothing else, which is the number that decides whether a static
address is worth taking. It is an observation at the caller's polling
resolution rather than an event timestamp, and a DHCP server that answers inside
one poll leaves `linkMs()` at zero.

**The cache is keyed on a hash of the SSID**, so editing `src/secrets.h`
invalidates it without anything having to remember to. It holds BSSID and
channel in RTC memory, which survives deep sleep but neither a reset nor a power
cycle -- the right boundary, since a board that has been switched off may well
be somewhere else. Every success rewrites it, so an access point that moved
fixes itself in one question; a failed attempt clears it, because an entry that
has just cost a full timeout is worth less than no entry at all.

**A drop after the association is a failure too.** `poll()` keeps watching once
it is `Online`, which is what lets the orchestrator apply the vision's rule that
a recording with nowhere to go is aborted rather than finished.

The driver is one association per wake, because the cache only means anything
across a deep sleep -- a single boot that connected ten times would measure ten
warm reconnects. Each wake is a row of a table kept in RTC memory and reprinted
as it grows: row one is always cold, since the reset that flashing or opening
the monitor causes clears RTC memory; then six timer cycles; then one cycle per
press, which is the pace the channel-move test needs. Every cycle chirps, ready
or error, so a run is legible on battery where there is no console at all.

### What it measured

Eleven wakes on the home network: one cold, five on the timer, then five by
hand, with the router switched off between the seventh and the eighth.

| Wake | Path | Link | Online | Boot to online |
| --- | --- | --- | --- | --- |
| 1, cold | scan | 101 ms | 3301 ms | 3454 ms |
| 2-7 | cached | 77-97 ms | 3208-3318 ms | 3261-3371 ms |
| 8, router off | cached, then two scans | -- | `NO WIFI` at 15005 ms | 15056 ms |
| 9, router back | scan | 1298 ms | 4408 ms | 4461 ms |
| 10-11 | cached | 78-88 ms | 3208-3258 ms | 3261-3311 ms |

- **The association is nothing and the address is everything.** A cached link
  comes up 77-97 ms after `begin()` and the IP arrives 3.11-3.24 s later, over
  thirty times as long, on every single wake. The BSSID cache the vision asks
  for does work and is worth having -- it is the difference between 87 ms and a
  scan, which was 101 ms once and 1298 ms when the network had just come back --
  but it is buying back a tenth of what the connect actually costs. The rest is
  [E6](experiments.md), written up from this run.
- **The budget arithmetic held to five milliseconds.** With the router off,
  cycle 8 spent 3 s on the cached AP and 6 s on each of two scans and reported
  `NO WIFI` at 15005 ms against a 15000 ms timeout, having made exactly the
  three attempts the windows allow.
- **A failed attempt clears the cache, and the next one pays for it.** Cycle 9
  is a cold scan rather than a fallback because cycle 8 had already thrown the
  entry away -- which is the intended trade: 1298 ms of scanning once beats 3 s
  of believing an access point that is no longer there on every wake.
- **The channel-move case arrived by itself.** The router came back up on
  channel 11 rather than the channel 1 it had been on, so cycle 9 found it by
  scanning, wrote channel 11 to the cache, and cycles 10 and 11 were fast again
  on the new channel. That is the scenario the step asked for, without anyone
  having to reconfigure a router.
- **Power save is not the DHCP wait.** `WiFi.setSleep(false)` before the
  association was measured over six wakes and changed nothing (3.11-3.15 s
  against 3.14-3.24 s), so the line came back out. Recorded under
  [E6](experiments.md) as a candidate ruled out.

**What this leaves for [S8](#s8----the-flow).** The association starts at the
wake and runs under the recording, so a question held for more than about three
and a half seconds finds the network already there and pays nothing. A short
one does not: a one-second hold reaches the release with roughly 2.4 s of
connect still to go, and that is time the user spends looking at a panel that
still says `LISTENING` before the upload has even started. It is the same gap
S8 has to decide what to fill, and now it has a number in it that is not the
backend's.

## S7 -- Upload and answer

`HTTPClient` POST of the buffer as `audio/wav`, no credentials
([D8](deferred.md)), plain HTTP ([D5](deferred.md)), `setTimeout(config::kResponseTimeoutMs)`.
The body is sent straight from PSRAM by pointer and length -- known length, so
Arduino's own client is enough until D4.

The answer is `{"response": "..."}`, parsed with ArduinoJson. Anything else --
non-200, unparseable, or valid JSON without `response` -- is one error to the
device.

The backend already answers this contract; a POST with an empty body comes back
`{"status":"ok","filename":"...","bytes":0,"response":"Received 0 bytes"}`.
That makes an empty POST a usable end-to-end smoke test before S3 is finished.

**There is no model behind it yet.** The answer is that fixed phrase, varying
only in the byte count, which is exactly enough to prove the chain: a number on
the screen that matches the length of what was recorded means the audio arrived
intact and the answer came back. Speech recognition is a backend job and comes
after the device works end to end.

**Verified by** the byte count on the screen matching the recording's length,
and by each error row of the vision's table provoked deliberately: wrong port
for `NO SERVER`, a backend returning 500, a backend returning `{}`.

### What it turned out to involve

`src/sticky_backend.h/.cpp`. `StickyBackend` is one call -- `ask(wav, bytes)`
returning one of five results -- and the orchestrator's half of it is two lines:

```
const StickyBackend::Result result = backend.ask(audio.wav(), audio.wavBytes());
if (result == StickyBackend::Result::Ok) screen.answer(backend.answer());
```

The body goes from PSRAM by pointer and length, as
[S3](#s3----capture-into-psram) arranged: `StickyAudio` reserved the 44 header
bytes at the front of its own allocation, so what goes on the wire is that
allocation and nothing copies a megabyte. `HTTPClient::POST()` takes a non-const
pointer although it only reads through it, so there is one `const_cast`, and it
is what keeps the copy from happening.

**The screen strings are not in the module.** `lastError()` returns a line for
Serial1 and the orchestrator maps the five results onto the vision's titles,
which is the division [S6](#s6----wifi) already drew between `NO WIFI` and
`StickyWifi`'s own strings. It keeps the vision's error table in one place
rather than in two halves that can drift apart.

**Two timeouts, because the two waits are different questions.** A backend that
is thinking gets `config::kResponseTimeoutMs`; a backend that will not accept a
connection gets `config::kBackendConnectTimeoutMs`, which is new here at 5 s.
HTTPClient defaults the second to 5 s as well, so the constant changes nothing
today and exists because [S7b](#s7b----cached-dhcp-lease) makes it load-bearing:
the failed connect is the only thing that can tell a stale cached lease from a
good one, so that number is exactly what the wake which notices will cost.

Three things about `HTTPClient` are worth knowing before reading a log from this
module:

- **Every failure to connect is reported as "connection refused."** Refused,
  unreachable, no route -- all `HTTPC_ERROR_CONNECTION_REFUSED`. So the clock is
  the only thing that separates a port that said no from an address that said
  nothing, and the module says which by comparing the elapsed time against the
  connect timeout. The difference is worth keeping: it is the shape S7b's stale
  lease will arrive in, and the two cost 200 ms and 5 s respectively.
- **`setTimeout()` takes a `uint16_t`.** A `config.h` that ever asked for more
  than 65.5 s would wrap to something short and the device would report
  `TIMED OUT` while the backend was still working -- a failure that looks like
  the network and is an edit. There is a `static_assert` on it.
- **A reply that stops halfway is a timeout, not a bad answer.** `getString()`
  drops the read error and returns what it has, so a body cut short would parse
  as malformed JSON and show `BAD RESPONSE`. The declared length is the only
  witness left, and comparing it against what arrived is what keeps the two
  apart.

The driver is the whole chain for the first time -- record, associate, POST,
draw -- with one thing the orchestrator will not have: **the target changes on
every question**. Each press walks one row of the vision's error table and the
next press walks the next, so the error paths are testable without a rebuild
between them; after the last row every press is the real endpoint again. Three
of the rows need a backend that misbehaves on purpose, which the prototype
server now has as `/audio/fault/500`, `/audio/fault/empty` and
`/audio/fault/slow` -- they drain the upload before answering, or the response
would close the connection under a device that is still sending and every fault
would read as `NO SERVER`. The other two rows need no backend at all: a closed
port and an address in TEST-NET-1.

**A release that arrives before the address is not a failure**, and the first
driver had it wrong. It asked `wifi.poll() != Online` after the recording
stopped, which is true both for an association that failed and for one that is
still three seconds from finishing -- so every question shorter than a DHCP
exchange ended in `NO WIFI` with a recording in hand and nothing wrong with the
network. Eight in a row on holds of 2.7 to 3.4 s, against the 3.2 s that
[E6](experiments.md) says an address costs. `NO WIFI` means the association's
own budget ran out, `config::kWifiConnectTimeoutMs`, and nothing else; a
question that finished early waits for the network it has already started.
[S6](#s6----wifi) had left exactly this on the table for S8 and it arrived a
step early, which is what a driver that runs the whole chain is for.

**The answer chirp moved in front of the refresh**, and the vision moved with
it. It used to sound when `screen.answer()` returned, on the principle that the
chirp means the answer is on the glass. On the device that reads as latency that
is not there: the panel is 2.4 s behind, the buzzer is the one channel that is
not, and a full refresh is legible well before it ends -- the text appears
inverted partway through. So the chirp now says the answer has arrived, the
refresh finishes while the user is still looking up, and every outcome chirps
before it draws rather than errors doing one thing and answers the other.

### What it measured

The run of record is fifteen wakes: the error table walked once, then nine real
questions with holds from 2.3 to 12.6 s. The table was walked three times in
all, across the runs that found the two bugs below.

| What | Screen | Round trip |
| --- | --- | --- |
| the real endpoint | the answer, byte count matching | 238 ms - 6848 ms |
| a closed port | `NO SERVER` | 56-193 ms |
| an address nothing answers at | `NO SERVER` | 5004 ms |
| `/audio/fault/500` | `SERVER ERROR 500` | 329-4274 ms |
| `/audio/fault/empty` | `BAD RESPONSE` | 475-4284 ms |
| `/audio/fault/slow` | `TIMED OUT` | 30503-32879 ms |

- **The chain is proved.** The byte count on the panel matched the recording
  every time -- 88620 bytes held for 2833 ms, 400428 for 12568 ms, and seven
  more in between -- which is what this step existed to show. Every row of the
  vision's error table was reached deliberately and showed the screen it is
  supposed to.
- **The two timeouts held.** The connect gave up at 5004 ms against 5000, and
  the response at 30621 ms against 30000 -- the extra being the upload, which
  happens before the wait starts.
- **The network is usable about 3.4 s after the press**, of which 3.2 s is the
  address ([E6](experiments.md)) and 190 ms is everything before
  `StickyWifi::begin()` gets called. So the wait after the release is 3.4 s
  minus the hold and nothing else: 1.11 s measured for a 2.34 s hold, 0.33 s for
  a 3.18 s hold, zero past about 3.4 s. A one-second question waits about 2.4 s,
  which is [S6](#s6----wifi)'s prediction arriving intact.
- **Release to answer on the glass is 3.5 to 9.5 s**, and it decomposes exactly:
  the wait for the network, the round trip, the 230 ms chirp and 2.4 s of
  refresh. The fastest question in the run still took 3478 ms to show anything,
  against a round trip of 238 ms -- so **the backend is not what the user is
  waiting for**, and S8's working screen is a question about the other three
  terms.
- **The round trip is not a number yet, and that is the finding.** 238 ms to
  6.8 s, and not because of size: 400428 bytes went up in 2876 ms while 112684
  took 6848 ms. The backend is not in it -- it swallows 120 KB in 4 ms over the
  LAN. Written up as [E7](experiments.md), which S8 needs before it can weigh
  2.4 s of refresh against a round trip it cannot predict.
- **Four questions could not reach a backend that was running**, twice in each
  of two runs, always giving up at exactly the connect timeout. The backend
  never saw them: no file appeared in its `recordings/` for either, while the
  wakes on both sides of them were answered normally. The host was not asleep --
  its own power log has no sleep event anywhere near, and an assertion holding
  it awake across the whole session. So it is the link, and the only instrument
  that can say more is the capture [E7](experiments.md) already asks for.

  **This is the one result that matters to [S7b](#s7b----cached-dhcp-lease)**,
  because a failed connect is what that step plans to read as a stale lease. On
  this network a healthy link produces one roughly every fifteen questions, and
  S7b would answer it by throwing away an address that was fine.

## S7b -- Cached DHCP lease

A letter rather than a number because this step did not exist when the order was
written: [E6](experiments.md) measured what an address costs and the answer was
large enough to be worth a step of its own. It lands here, straight after S7 and
before the flow, for one reason -- **the upload is the probe**. A cached address
that has gone stale shows up as a TCP connect that fails, which is something S7
does on every question anyway, so this step needs no probe of its own and no
extra second on the happy path.

What it delivers: the lease from the last successful wake kept in RTC memory
next to the BSSID, installed with `WiFi.config()` before the association, and
dropped the moment the question it is carrying cannot reach anything.

- The saving is 3.1 s per question: 178-226 ms from the top of `setup()` to a
  usable network, against 3268-3427 ms for DHCP ([E6](experiments.md)).
- **The detector is not as clean as it looked.** [S7](#s7----upload-and-answer)
  found that a healthy network on this desk produces a connect that fails
  outright about once every fifteen questions, for reasons that are
  [E7](experiments.md)'s to find. A lease dropped on one of those costs the
  3.2 s this step exists to save, on a wake where nothing was wrong. It does not
  sink the step -- the cost of a false positive is exactly the cost of not
  having the cache at all -- but the rule below wants a second failure behind it
  rather than a first, and E7 comes before this step for that reason.
- **Failure is the only detector, and it has to be wired to the upload.**
  `StickyWifi` cannot tell a good address from a stale one by itself -- both
  install in 40 ms and neither says anything. So the rule is the one E6's rig
  demonstrated: when the connection to the backend fails, drop the lease, start
  DHCP, and let the question have its answer 3.2 s later. A stale lease costs
  6.3 s on the wake that notices and nothing afterwards.
- **A lease has a life and the firmware can count it.** This network offers
  86400 s. The RTC counter survives deep sleep -- the correction
  [S2](#s2----button) made to E1 is the same fact -- so an entry can be aged out
  rather than trusted until it fails.
- Nothing here is a claim on a fixed address: the device only ever reuses what
  the server gave it, which is what makes this different from a static IP.

**Verified by** the E6 numbers coming back from the firmware rather than a rig,
and by a question that starts with a stale lease still getting its answer.

## S8 -- The flow

`main.cpp` becomes the orchestrator and the demo code goes: wake, latch, button,
microphone, capture task, ready chirp, WiFi and the Listening screen, release,
stop, upload, answer or error chirp, draw, deep sleep.

- The order at the front is load-bearing and measured: capture starts *before*
  the chirp, because the chirp means "the microphone is live"
  ([E1](experiments.md), [E3](experiments.md)).
- **The pre-clear should not happen on every wake.** `setupDisplay()` currently
  does a `fillScreen(TFT_WHITE)` and a full refresh at boot, which is one to two
  seconds spent flushing a panel the Listening screen is about to overwrite
  anyway. It is needed on a cold start, where the controller's previous-image
  RAM is unknown; `stickyPower::wokeFromDeepSleep()` already tells the two
  apart. S5 measured the cost at 2373 ms and the driver already branches on it.
- **Decide what the device does between the release and the answer.** The
  vision's step 6, and the one question it leaves open. Until the answer is
  drawn the panel still reads `LISTENING`, which stops being true the moment the
  button comes up, and the answer chirp only marks the end of the gap rather
  than filling it -- [S7](#s7----upload-and-answer) moved it in front of the
  refresh, which buys back 2.4 s of the wait but none of the rest. This is the step where it can finally be judged, because it
  is the first time a real wait exists to sit through.

  It is the only transition that costs the user anything: released, draw,
  upload, wait, draw again, so a full refresh puts its 2.4 s in front of every
  answer -- to show a screen a fast backend may not leave up long enough to
  read. Three shapes, in rising order of work:

  - a screen of its own, `StickyScreen::working()`, at the full 2.4 s;
  - a fourth buzzer pattern on release and no screen at all, which costs
    milliseconds but leaves the panel lying until the answer lands;
  - a partial refresh of the word alone -- [D6](deferred.md), which would also
    be the first time the partial-refresh correction in `src/sticky_epaper.h`
    has ever run on the device, and whose cost on this panel is unmeasured.

  Whichever is chosen, the number that decides it is how long the backend
  actually takes, which [S7](#s7----upload-and-answer) is the first step to
  produce -- and only once there is a model behind it rather than a fixed
  phrase.
- Every exit is deep sleep with the latch held, including the error paths and
  the discarded tap.

**Verified by** the whole interaction, on battery, with the cable out.

## S9 -- Re-run E1

[E1](experiments.md) asks for this explicitly: the boot stage scales with the
image, the rig's image was smaller than the firmware's, and the numbers in
experiments.md are from a rig. Re-run it on the finished firmware, record both
image sizes, and update the result there rather than here.

---

## Settled while planning

Four questions came out of writing this down. All four are answered; the
reasoning lives where it belongs and is summarised here so nobody reopens them
by accident.

- **The ready chirp sounds before a tap is known to be a tap**, and that is
  accepted -- [D7](deferred.md) has the timings and the two worse alternatives.
  The interaction that creates the case may not survive the UI/UX pass anyway
  ([D9](deferred.md)). (S2)
- **A failed buffer allocation is `NO MEMORY`**, now a row in the vision's error
  table. (S3)
- **Answers run off the right edge.** [D2](deferred.md) stands: the backend
  returns a fixed phrase, so there is nothing to wrap until a model is behind it,
  and the UI/UX pass comes after the prototype works end to end. (S5)
- **How long the upload takes does not matter yet.** Nothing waits on the
  answer, so the number has nowhere to land until [D4](deferred.md) is being
  weighed for real. (S7)
