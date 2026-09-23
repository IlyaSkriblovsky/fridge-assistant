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
| S7b | Cached DHCP lease | Three seconds off every question | Done |
| S8 | The flow | Wake, record, ask, show, sleep | Done |
| S9 | Re-run E1 | Wake latency of the real firmware | Done |
| S10 | Display task | The panel off the orchestrator's thread | Done |
| S11 | Streaming upload | The body going up under the hold | Done |
| S12 | DNS server | Names looked up past a router that stops answering | Built, not yet run on the device |
| S13 | Fonts | Cyrillic, Greek and typography on the panel; D1 gone | Done |
| S14 | Word wrap | An answer of several lines, laid out on the panel; half of D2 gone | Done |
| S15 | Idle dashboard | Backend frame, telemetry, post-answer wait and scheduled refresh | Implemented; hardware acceptance pending |

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

`src/sticky/buzzer.h/.cpp`. Three patterns, from the vision's table: ready (two
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

`src/sticky/button.h/.cpp`. The AI button on GPIO4, active low, internal
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
- `src/recording.h/.cpp` owns the buffer: one `heap_caps_malloc(..., MALLOC_CAP_SPIRAM)`
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

`Recording` is `begin(sampleRate)`, three lines per chunk, and `wav()`:

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

`src/capture.h/.cpp`. `Capture` is `start()`, `abort()`,
`finished()`, `wait()` and `stopReason()`, and the orchestrator's half of it is
five lines:

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

`src/sticky/screen.h/.cpp`: `listening()`, `answer(text)`, `error(title, detail)`.
A small interface on purpose -- D4 moves the display into its own task later,
and that is cheap only if nothing else calls the panel directly.

- Full refresh on all three transitions ([D6](deferred.md)).
- Originally used the library's default orientation, with buttons along the
  bottom edge. Superseded on 2026-09-21 after use: `StickyScreen::begin()` now
  sets rotation 2 (180 degrees), putting the buttons along the top edge.
  Build and USB upload succeeded on 2026-09-21; flash contents were verified
  and the device reset. The user confirmed on the device that the new
  buttons-up orientation works correctly.
- Only `LOAD_GLCD` and `LOAD_GFXFF` are compiled into Seeed_GFX2, so the answer
  is a FreeFont through `setFreeFont()`. Font 1 at 8x8 is unreadable on a 4"
  panel without scaling.
- ASCII only (D1, since paid off in [S13](#s13----fonts)). A non-ASCII byte
  should degrade visibly but not draw garbage.
- No battery level ([D3](deferred.md)).
- Error strings come from the vision's table: `NO WIFI`, `NO SERVER`,
  `SERVER ERROR` plus status, `BAD RESPONSE`, `TIMED OUT`, `NO MICROPHONE`,
  `NO MEMORY`.
- **No word wrap** ([D2](deferred.md)). A long answer runs off the right edge
  and that is accepted: the backend returns a fixed phrase, so there is nothing
  to wrap until there is a model behind it. *Paid off in
  [S14](#s14----word-wrap), once there was.*

**Verified by** the user looking at the panel. Worth driving all three screens
from a temporary `loop()` before the flow exists, since this is the one step
where the failure mode is cosmetic and only a human can see it.

### What it turned out to involve

`StickyScreen` is `begin()`, `clear()` and the three screens, and it is the only
place `Seeed_GFX` is touched -- `src/sticky/epaper.h` is now included by
`src/sticky/screen.cpp` rather than by `main.cpp`, so the rest of the firmware never
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
else, so a Russian word that slips past the backend's transliteration (D1)
comes out as one `?` per letter instead of two or three. It is the only part of
the module that can be reasoned about away from the device, which is why it is
a free function rather than a private method. Sanitising also keeps
`textWidth()` honest: it walks bytes while `drawString()` decodes UTF-8, so the
two disagree about the width of anything multi-byte, and every datum but
`TL_DATUM` is computed from that width.

*[S13](#s13----fonts) deleted this function and stopped using `drawString()`.
The disagreement it worked around is real and permanent -- sanitising only hid
it while every string was ASCII, and `src/text.cpp` measures instead.*

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

`src/wifi_link.h/.cpp`. `WifiLink` is `begin()`, `poll()` and `end()`, and
the orchestrator's half of it is three lines:

```
wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword);
...                                     // the Listening screen, the recording
if (wifi.poll() != WifiLink::State::Online) capture.abort();
```

**The class is a clock, not a state machine over `WiFi.status()`.** The attempt
is a series of windows inside one budget: the cached AP gets
`config::kWifiCachedApAttemptMs`, and after that each fresh scan gets 6 s, until
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

`src/backend.h/.cpp`. `Backend` is one call -- `ask(wav, bytes)` returning one
of five results -- and the orchestrator's half of it is two lines:

```
const Backend::Result result = backend.ask(audio.wav(), audio.wavBytes());
if (result == Backend::Result::Ok) screen.answer(backend.answer());
```

The body goes from PSRAM by pointer and length, as
[S3](#s3----capture-into-psram) arranged: `Recording` reserved the 44 header
bytes at the front of its own allocation, so what goes on the wire is that
allocation and nothing copies a megabyte. `HTTPClient::POST()` takes a non-const
pointer although it only reads through it, so there is one `const_cast`, and it
is what keeps the copy from happening.

**The screen strings are not in the module.** `lastError()` returns a line for
Serial1 and the orchestrator maps the five results onto the vision's titles,
which is the division [S6](#s6----wifi) already drew between `NO WIFI` and
`WifiLink`'s own strings. It keeps the vision's error table in one place
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
  `WifiLink::begin()` gets called. So the wait after the release is 3.4 s
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

  **[E7](experiments.md) has since answered it.** 128 KB goes up in 660 ms and
  the spread is one lost acknowledgement: the device keeps 5744 bytes in flight,
  about one ACK in 110 does not come back, and with nothing behind it to cover
  the loss the window stops for a retransmission timeout of one to two and a
  half seconds. An upload is twenty-two bursts, so roughly one question in four
  pays and the payload has nothing to do with which -- which is this row of the
  table, exactly.
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

  **[E7](experiments.md) could not reproduce it** -- seventy-one connections,
  every SYN answered inside a millisecond, none retransmitted -- and it corrected
  the inference above on the way. "No file appeared in `recordings/`" only means
  the POST never completed; a file is written when the upload finishes, so it is
  silent about whether the SYN ever arrived. The backend was not the witness this
  row took it for.

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
- **The detector is not as clean as it looked, and E7 did not clean it.**
  [S7](#s7----upload-and-answer) found that a healthy network on this desk
  produces a connect that fails outright about once every fifteen questions. A
  lease dropped on one of those costs the 3.2 s this step exists to save, on a
  wake where nothing was wrong. [E7](experiments.md) went looking and found
  nothing: seventy-one connections, every SYN answered in 0.1 to 0.7 ms, none
  retransmitted. So the false positive is not a routine property of this
  network -- but it happened four times in two S7 runs and remains unexplained,
  and E7 names a mechanism that would do it (a SYN-ACK lost the way it found
  ACKs being lost costs a 3 s retransmit against a 5 s budget) without proving
  it. **The rule below still wants a second failure behind it rather than a
  first**, and it is cheap: the cost of asking twice is one extra connect
  timeout on a wake that was already going to be slow.
- **Failure is the only detector, and it has to be wired to the upload.**
  `WifiLink` cannot tell a good address from a stale one by itself -- both
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

### What it turned out to involve

`WifiLink` gains a second entry in RTC memory beside the AP: the address, the
gateway, the mask, the DNS server, the life the server offered and the RTC
counter at the moment it was granted. `begin()` installs it with
`WiFi.config()` in the window between `WiFi.mode()` and the association -- the
netif has to exist, and the station has to know it is not asking for an address
before it associates -- and that two-line window is the whole saving.

**The entry is tied to the SSID and not to the BSSID**, and it is a separate
entry rather than a wider one. A lease belongs to the network behind the radio,
so the same subnet reached through a different access point of the same network
is still the right subnet; and the two are dropped for different reasons at
different moments -- an association that failed says nothing about the address,
an address that reaches nothing says nothing about the radio.

**Half the offered life is the ceiling, and it is DHCP's own T1** -- the point
at which a client that had one running would be renewing rather than using the
address. The firmware has no client running between questions, so the same
boundary is where it stops reusing and asks properly, which keeps the device
inside the contract the server wrote rather than inside an interval invented
here. The number comes from the lwIP client's `offered_t0_lease` through
`netif_dhcp_data()`, because arduino-esp32 exposes no accessor for it; a wake
whose client never said is not written to the cache at all, and says so in the
log rather than guessing. The age is counted on the RTC counter, since
`esp_timer` restarts on every wake -- [S2](#s2----button)'s correction to
[E1](experiments.md), reused.

**Failure is the only detector, and `Backend` had to learn to report it.**
The module already knew the difference -- HTTPClient calls every failed connect
"connection refused", so the clock is what separates a port that said no from an
address that said nothing -- but it kept the difference in a log string.
`unreachable()` is that same test as a value, and it is the only distinction
inside `NO SERVER` the device can act on: a refusal proves something is at the
address and therefore that the address works, while silence is also the shape of
a lease that has outlived its network.

**The rule asks twice.** A lease is dropped on the second unanswered connect,
not the first, which is what the step planned against [S7](#s7----upload-and-answer)'s
one-failure-in-fifteen. It costs one connect timeout on a wake that was already
going to be slow and it keeps a false positive from throwing away a working
address. The rule lives in the orchestrator, because it spans both modules and
neither half can see it alone.

**The renewal cannot be waited on by the status or by the address.**
`WiFi.config(INADDR_NONE, ...)` restarts the client without touching the
association, but `WiFi.status()` is still the `WL_CONNECTED` the discarded
address set and has no event coming to replace it -- and the address the server
hands back is usually the one just dropped, so "it changed" waits for something
that is not going to happen. E6's rig got away with the second test because its
stale lease was from a subnet that could never come back. The client's own
`DHCP_STATE_BOUND` is the only witness that answers the question being asked.

**The first run of the driver measured the panel.** Four wakes reported an
installed lease as costing 2642, 2643, 2642 and 2643 ms -- a fixed timer, not a
network. Between `wifi.begin()` and the loop that polls it stands the LISTENING
refresh, and `poll()` cannot time something that happens while its caller is
busy for two and a half seconds. [S7](#s7----upload-and-answer) never saw it
because 2.4 s of panel fits inside 3.2 s of DHCP. So `linkMs()` and the new
`onlineMs()` are now timestamps taken in the WiFi task from
`ARDUINO_EVENT_WIFI_STA_CONNECTED` and `..._STA_GOT_IP`, `elapsedMs()` is kept
as what the polling thread believed, and the driver prints both. **A number read
off a poll is a number about the poller.**

The driver walks five rows, one per press, and two of them exist to keep the
detector honest: a closed port, which must be refused and must leave the lease
alone, and the row after it, which has to still find the lease there. The stale
row is the one failure in the step that cannot be reached by waiting -- a lease
only goes stale when the network changes underneath it -- so
`WifiLink::spoilLease()` moves the cached entry onto a subnet the device is
not on, on the wake before. It is the one seam in the module that the firmware
never calls, and it is deliberately not a lease setter: it can only spoil what
the server already gave.

### What it measured

Two runs of six wakes, the second after the instrument was fixed. The table is
the second.

| Mode | Link | Address | Top of `setup()` to a usable network |
| --- | --- | --- | --- |
| DHCP | 85 ms | 3332 ms | 3546 ms |
| cached lease | 78-83 ms | the same millisecond | 302-307 ms |
| stale lease | 78 ms | the same millisecond | 302 ms, and then 13.2 s |

- **The saving is 3.24 s a question**, which is [E6](experiments.md)'s 3.1 s
  arriving intact. The firmware's 302-307 ms against the rig's 178-226 ms is not
  the network: it is the 214-228 ms of microphone, buffer, capture task and
  ready chirp that the firmware does before `wifi.begin()` and the rig did not.
- **With the address installed there is no DHCP step at all.** The link and the
  address are the same millisecond on every cached wake -- 79/79, 82/82, 83/83 --
  while the DHCP wake has 85 ms of link and then 3.2 s of exchange. That is E6's
  decomposition seen from the firmware: the association was never the expensive
  half.
- **The lease's own numbers came back right.** 86400 s read from the client, and
  an age that counted 4, 6, 16, 25, 26 and 35 s across six deep sleeps -- so the
  entry can be aged out rather than trusted until it fails.
- **A refusal leaves the lease alone.** The closed port was refused in 58 ms and
  the next wake found the same lease, ten seconds older. The detector is the
  connect timeout and not `NO SERVER`, which is the distinction the step
  depended on.
- **A stale lease costs 13.2 s and still answers.** 10007 ms of two connects
  that answered nothing, 3151 ms of DHCP, and then the question went up and came
  back normally. E6 predicted 6.3 s for a rule that drops on the first failure;
  the second ask is the other half, and it is the price of not throwing away a
  working address.
- **What the polling thread believed was 2641-2644 ms on every cached wake**, to
  the millisecond, which is the LISTENING refresh and nothing else. It is now
  printed beside the real number rather than instead of it.
- **The address has stopped being what a question waits for.** `wait` -- release
  to a usable network -- is 0 ms on every cached wake against 278 ms on the DHCP
  one, and the release-to-upload gap is 69-76 ms. Those holds were 3.3 to 4.6 s,
  so the refresh had finished long before the button came up; a question shorter
  than about 2.6 s would now wait for the panel instead, which is
  [S8](#s8----the-flow)'s problem and is written up there.

## S8 -- The flow

`main.cpp` is the orchestrator and the demo code is gone: wake, latch, button,
microphone, capture task, ready chirp, WiFi and the Listening screen, release,
taken chirp, the working screen, upload, answer or error chirp, draw, deep
sleep.

- The order at the front is load-bearing and measured: capture starts *before*
  the chirp, because the chirp means "the microphone is live"
  ([E1](experiments.md), [E3](experiments.md)).
- **The pre-clear does not happen on every wake.** A `fillScreen(TFT_WHITE)` and
  a full refresh at boot is one to two seconds spent flushing a panel the
  Listening screen is about to overwrite anyway. It is needed on a cold start,
  where the controller's previous-image RAM is unknown;
  `stickyPower::wokeFromDeepSleep()` tells the two apart.
- **What the device does between the release and the answer is settled**, and it
  is the vision's step 6 closed. The gap is covered twice over: a fourth buzzer
  pattern the moment the button comes up, and a partial refresh of the word
  alone -- `LISTENING` becomes `WORKING` -- which is [D6](deferred.md) arriving
  a step early and for a reason.

  The three shapes were a screen of its own at the full 2.4 s, a chirp and no
  screen at all, and the partial. [E7](experiments.md) ruled out the first: the
  round trip is about 0.5 s three times out of four and 1.5 to 5.5 s the fourth
  time, so a full refresh would put 2.4 s in front of a wait that is usually
  shorter than itself, to show a screen a fast backend would not leave up long
  enough to read. What was owed before the third could be picked was its cost on
  this panel, which nobody had measured and which is now 990 ms -- **and the
  reason it could be picked without an experiment first is that this step has to
  run on the device anyway**, so the measurement is a line in the log rather
  than a rig.
- **One full refresh per question, and it is the first one.** Having measured
  the partial the step went back and made the answer and the errors partial too,
  which is the arrangement the panel ends up with: `listening()` full,
  everything after it differential. It is not a preference. A partial update is
  differential against what the controller has been told is on the glass -- the
  shadow in `src/sticky/epaper.h`, allocated fresh on every boot and seeded
  white -- and after a deep sleep the glass still holds the last answer while
  the firmware has been told nothing about it. Only a full refresh drives every
  pixel whatever it was, so exactly one is needed to reconcile the two, and it
  may as well be the one that runs under the recording where it costs nothing.
  `clear()` is the cold-start form of the same reconciliation.

  **It also fixes the ghosting depth at two, by construction.** Full, partial,
  partial, sleep -- there are never more than two partial refreshes between two
  full ones, whatever the sequence of questions, so nothing accumulates across a
  run and neither this arrangement nor the one [D6](deferred.md) leaves it with
  asks how many partials the panel will take. The full refresh is not moving off
  the wake, which is the only thing that would have let partials chain.
- Every exit is deep sleep with the latch held, including the error paths and
  the discarded tap.
- **Two things came across from [S7b](#s7b----cached-dhcp-lease)'s driver rather
  than dying with it.** The stale-lease rule is `askRenewingStaleLease()` -- ask
  again, then drop the lease, take an address and ask once more -- and it
  belongs in the orchestrator because it spans `Backend` and `WifiLink` and
  neither half can see it alone. The pre-clear branch on
  `stickyPower::wokeFromDeepSleep()` is the other.
- **`WifiLink::spoilLease()` went with the driver**, as it was meant to. It was
  the one seam in a permanent module that the firmware never called, kept
  through S7b because the rule it tested was moving into new code and wanted
  walking once in its new home. It has walked.

**Verified by** the whole interaction on the device: nineteen questions of
varying length, the panel checked by eye through a run of consecutive questions
with one full refresh apiece -- every transition clean, no smear, no residue --
and the numbers below off Serial1.

### What it turned out to involve

**The working screen is two words in one screen, not two screens.**
`listening()` and `working()` draw a single word in the same face at the same
size, centred, so the strip of panel the word occupies is a property of the face
and not of the word -- `StickyScreen::wordBand()` is that strip, and `working()`
repaints it and refreshes it alone. Nothing outside the band differs between the
two screens and nothing outside it is sent to the controller a second time. The
band is `fontHeight()` plus a small margin, which is 136 rows of 480: the line
advance is larger than the ascent-plus-descent that `drawString` centres glyphs
within, so a band of that height clears the word top and bottom without
measuring a single glyph.

**The partial-refresh correction in `src/sticky/epaper.h` has now run.** Written
at S5 and never exercised until this step -- [D6](deferred.md) said as much --
it supplies the previous-image plane that `Panel_EPaper::updatePartial()` never
pushes. It works: the old word goes and the new one arrives, with no smear and
no residue of `LISTENING` inside the band. Checked at the panel over a run of
questions, which is the only way that question can be answered.

**Two log lines were wrong on the first run, and both were the same mistake.**
A wait broken into parts is only a breakdown if the parts do not overlap:

- *Was the release inside the Listening refresh?* was answered by comparing how
  long ago the release was against how long the refresh took, which is not that
  question and got it backwards on every hold. The refresh's own end timestamp
  is what answers it.
- *How much of the wait was the network?* was the address's arrival measured
  from the release -- and on a wake with a lease the address arrives while this
  thread is still inside the Listening refresh, so those milliseconds were being
  counted twice. One question reported 2051 ms of leftover refresh and 2625 ms
  of network inside a total of 3382 ms. What the question actually waited for is
  the poll loop's own duration *after* the panel let go, which is zero on every
  cached wake; the radio's view stays on the line above it, where it answers a
  different question and is right about it. It is
  [S7b](#s7b----cached-dhcp-lease)'s rule read from the other end -- a number
  read off a poll is a number about the poller, and sometimes the poller is what
  you meant to measure.

**The answer chirp was inside the panel's number.** Every chirp sounds before
its screen, deliberately, so the 230 ms of the answer pattern sits between the
release and the refresh -- and timing the two together reported every full
refresh in this firmware as 2619-2620 ms against the 2375-2377 ms
[S5](#s5----screens) measured the same screen at. The gap was the buzzer and not
the panel: S5's number stands, and so does the 2398-2403 ms in the table, which
is a full refresh of the same firmware timed with the chirp outside it. The
firmware now ends the wait where the chirp starts and times the refresh from
where it begins.

**`finish()` is the only exit and it joins the capture task on a bounded wait.**
The task owns the microphone until it is joined, so the abort has to come before
`mic.end()`; the bound is there for one case, a `start()` that created its
semaphore and then failed to create the task, which would otherwise leave a
question hanging with the rail latched.

### What it measured

Nineteen questions on the device, holds from 0.2 to 5.0 s. The panel is
deterministic to the millisecond partially as well as fully, exactly as
[S5](#s5----screens) found:

| Transition | Window | Refresh |
| --- | --- | --- |
| `clear()`, cold start only, full | 480 rows | 2386-2389 ms |
| Listening, full | 480 rows | 2398-2403 ms |
| working, **partial** | 136 rows | **990-991 ms** |
| answer, **partial** | 480 rows | **1343-1346 ms** |

- **A partial refresh does not vary at all.** Fourteen of them: 990 or 991 ms
  over the word's 136 rows, 1343 to 1346 ms over all 480. The panel is as
  deterministic partially as it is fully.
- **The saving does not collapse as the window grows**, which is the thing worth
  knowing and was not obvious from the band alone. A partial of the whole panel
  is 1344 ms against 2400 ms full -- 56% of the price for 100% of the area. Two
  points is enough to take the number apart, and the arithmetic is on measured
  values rather than on a model: (1344-990)/(480-136) is **1.03 ms a row**, so
  the fixed part of a partial is 990 - 136x1.03 = **850 ms** and the fixed part
  of a full refresh, at the same per-row cost, is 2400 - 494 = **1906 ms**. The
  1056 ms between them is waveform and nothing else; both push two planes of the
  same size, so the transfer cancels. The per-row millisecond is not SPI either
  -- it is the byte-at-a-time bit reversal in `Panel_EPaper::pushNewColorsFlip`
  and in `pushInvertedRows`, which is CPU.
- **`listening()` is the one transition that cannot have any of this**, and it
  is also the one still costing a short question the most: the leftover refresh
  was 1809 ms on a 1.0 s hold and 1239 ms on a 1.6 s one. It stays full for the
  reason in the note above, and that reason does not go away -- the screen left
  on the glass between questions is meant to be an idle screen of the wider UI's
  own, whose content a wake has no way to rebuild, so there is never anything to
  seed the shadow from. What takes those milliseconds off the critical path is
  not a cheaper waveform but [D4](deferred.md)'s display task: behind a queue the
  orchestrator stops sitting inside the refresh, sees the release when it
  happens and starts the upload while the panel catches up. D6 has the argument
  now that this step has the numbers for it.
- **The largest thing a short question waits for is the Listening refresh it is
  still inside.** 1680 ms of it on a 1.2 s hold, 2051 ms on a 0.8 s one. The
  working screen and the round trip together are about 1.5 s, so the leftover
  refresh is larger than both for anything held under about two seconds. A hold
  longer than the refresh pays nothing: 0 ms on every hold past 3.5 s.
- **The network has left the wait entirely.** Zero milliseconds on every wake
  with a lease, because the address lands 78-91 ms into the wake and this thread
  does not reach the poll loop for another three seconds. S7b's 3.24 s is intact
  and the panel has inherited the whole of what used to be the radio's.
- **The round trip behaved the way [E7](experiments.md) said it would, and the
  larger uploads made the point twice over.** Nine uploads of 24 to 128 KB came
  back in 251, 404, 408, 464, 515, 612, 955 and 1008 ms with one of 3044 ms --
  one stall in nine. Five later ones of 31 to 157 KB came back in 282, 1777,
  1914, 2867 and 4454 ms: four stalls in five. E7 puts an upload at about
  twenty-two windows with one in four stopping for a retransmission timeout, so
  a 157 KB upload has more windows and more chances, and that is what the second
  set shows. The 31 KB one in the middle of it went in 282 ms.
- **The release-to-chirp wait is 2.0 to 5.6 s**, and the shape of it is the
  whole argument for [D4](deferred.md)'s display task. On a long hold it is the
  working screen plus the round trip -- 990 ms of panel in front of anything
  from 282 ms to 4.5 s of network. On a short one the leftover Listening refresh
  is in front of both. **Every millisecond of the panel in that number is a
  millisecond a display task would remove**, because nothing about the upload
  needs this thread; that is about a second a question on a long hold and closer
  to two and a half on a short one, which is more than the streaming upload D4
  is nominally about.
- **The answer's own refresh is not in that number and is worth having anyway.**
  It comes after the chirp, so making it partial does not shorten a wait -- what
  it shortens is the time to a fully drawn panel and the time to sleep, by
  1056 ms a question. That is battery rather than latency, which is a different
  argument for the same change.
- **The microphone is live 203-204 ms after a wake** and 303 ms after a
  power-on, which is [S9](#s9----re-run-e1)'s number arriving early and from the
  wrong end: it is measured from the top of `setup()` and E1 measures from the
  button.

  *Corrected at [S9](#s9----re-run-e1).* Wrong at this end too. The log line was
  timed after `stickyBuzzer::ready()` returned, and the chirp is 110 ms of
  blocking, so 203 ms was the end of the chirp. The microphone was live and the
  chirp starting at 93 ms into `setup()` -- 43 ms since S9 took a stray
  `delay(50)` out -- and the line is now timed where the chirp starts.

## S9 -- Re-run E1

[E1](experiments.md) asks for this explicitly: the boot stage scales with the
image, the rig's image was smaller than the firmware's, and the numbers in
experiments.md are from a rig. Re-run it on the finished firmware, record both
image sizes, and update the result there rather than here.

The result is there, in
[E1's re-run](experiments.md#re-run-on-the-finished-firmware-2026-09-18). **The
firmware chirps 104.3 ms after the wake event, every time to within 0.2 ms**:
61.2 ms of boot, 41.3 ms of microphone and 1.7 ms of everything else. What
follows is what the step involved on this side.

### What it turned out to involve

**The premise had dissolved before anything was flashed.** E1's rig compiles
every module in `src/` but `main.cpp`, and those have grown into WiFi, HTTP and
the panel since E1, so the rig now copies 79588 bytes of IRAM and 22812 of DRAM
on a wake against the firmware's 79692 and 23116. What it still cannot have is
`main.cpp`, whose globals are constructed inside the boot being measured.

**So the rig for this step is the firmware.** `exp_e1_firmware` builds
`main.cpp` as it is and wraps five of its calls at link time with `--wrap`:
`setup()`, `holdLatch()`, `StickyMic::begin()`, `stickyBuzzer::ready()` and
`stickyPower::deepSleep()`. The firmware is not edited for it, the image copies
exactly the bytes the firmware does, and a timer wake is the firmware handling a
tap -- which it already knows how to do -- so every row is a whole run from the
wake to the sleep. It is the first rig that measures the firmware rather than
the firmware's modules, and the shape is reusable: anything that can be timed at
a call boundary in `main.cpp` can be timed without touching `main.cpp`.

**The instrument had to be fixed before the numbers meant anything.** The first
run worked the wake deadline out, as E1 always had, and reported the firmware
booting 5 ms faster than E1's rig. The deadline is in a register, and read from
there the boot is 61.2 ms on the firmware and 61.8 on the rig with no spread at
all; the 5 ms, and every millisecond of E1's own spread, was a calibration
difference in the working-out. [E1](experiments.md) has the correction, and the
old rig reads the register now too.

**Two changes to `main.cpp`, both on the wake path:**

- **`delay(50)` after `Serial1.begin()` is gone.** It came with the first demo,
  waited for nothing, and was a third of the wake-to-chirp time -- 51.5 ms of a
  stage that is 1.3 ms without it. E1's rig never saw it because the rig starts
  `Serial1` after its capture. It is exactly the kind of number E1 said to look
  for: the wake path is delays this project chose, not silicon.
- **The "microphone live" line is timed where the chirp starts.** It was timed
  after the chirp returned, which is how [S8](#s8----the-flow) came to record
  203 ms for a chirp that started at 93. S8 carries the correction.

**A tap never polls the network**, which showed up as the first real question
after the timer phase paying for a scan and DHCP. The orchestrator's poll loop
runs only after the Listening refresh, and a tap has finished long before that,
so twenty taps in a row never reached `Online` and never cached an access point
or a lease. Harmless -- a tap has nothing to send -- and the cache from the last
real question survives any number of them, since a tap neither writes nor clears
it.

**The panel is 100 ms lower on every screen than S8's table**: 2300 ms for the
Listening refresh, 889 for the working screen and 1243 for the answer, against
2398-2403, 990-991 and 1343-1346. That is [E8](experiments.md)'s `sleep()`
override arriving in the firmware's own log, 100 ms a refresh as E8 measured it
on its rig.

### What it measured

Three runs of the wrapped firmware -- one before the change, two after -- and two
of E1's rig for control, all on one afternoon:

| | Before | After |
| --- | --- | --- |
| the wake to `setup()` | 61.2 ms | 61.2 ms |
| `setup()` to the ready chirp | 93.0 ms | 43.0 ms |
| **the wake to the ready chirp** | **154 ms** | **104.3 ms** |

- Forty timer wakes after the change, 104.2 to 104.4 ms from the wake event to
  the chirp. Five real questions on the button, short and long, came out at
  43.0 ms from `setup()` to the chirp on every one, and all five were answered
  with the byte count matching.
- The boot is the same on the firmware as on a rig, to 0.6 ms, and does not
  vary from wake to wake at all. Nothing the firmware has grown into since E1
  costs anything before `setup()`.
- **What is left is two things**: 61 ms of boot, and 41 ms of microphone, of
  which 30 ms is the 24 ms settle discard rounded up to the DMA's 15 ms blocks
  and 10 ms is the rail's `delay(10)`. Both levers are written up in E1 and
  neither is taken here -- the first changes what the chirp promises and the
  second means taking E3 again.
- **Deep sleep stays.** Light sleep would buy about a tenth of a second now,
  which is less than before and still not worth idle current.

## S10 -- Display task

The panel on a thread of its own, which is what [D6](deferred.md) still owes and
what [D4](deferred.md) names as its precondition. It comes before the streaming
upload rather than inside it, for two reasons that are both measured:

- **It is worth more than the upload.** [S8](#s8----the-flow) put the
  release-to-chirp wait at 2.0 to 5.6 s, and on every hold part of that is the
  panel: the 889 ms working screen always, and on a hold shorter than the
  Listening refresh whatever is left of that too -- 1.2 to 2.1 s, measured. None
  of it needs this thread. That is about a second off a long question and two to
  three off a short one, before a byte of the upload moves.
- **The upload cannot stream without it.** On a cached wake the address arrives
  while the Listening refresh has barely started, and the orchestrator then sits
  inside that refresh for two more seconds, unable to write anything. S11 on a
  single thread would send nothing until the refresh let go, which on a short
  question is after the release -- the POST it was meant to replace.

It can also be checked by itself, which is what the step order asks for: the
upload stays a single POST after the release here, so the only thing that
changes is who waits for the panel.

What it delivers: one task that owns `StickyScreen`, and an orchestrator that
posts screens instead of drawing them. The wait after the release keeps the
chirp, the network and the round trip, and loses the panel.

- **The task wraps `StickyScreen`; it does not replace it.** `screen.h` was kept
  four calls wide for this day, and the four become messages. The task is logic
  rather than board -- the rule that put `Capture` at the top of `src/` and
  `StickyMic` in `sticky/` -- so it is a new module next to `Capture`, and after
  it nothing but the task touches the panel.
- **It wants no priority to speak of.** A refresh is mostly the library waiting
  on BUSY in `vTaskDelay()` -- D4 has the arithmetic -- so the task is blocked
  for nearly all of its life. What it does spend CPU on is the drawing before a
  refresh, about 260 ms for a full screen, and the 222 ms plane push inside one
  ([E8](experiments.md)). Well below capture; the core is chosen on the device,
  against the capture task's slow-chunk count.
- **A queue of one, with replacement** -- D6's rule. A screen posted while
  another is still waiting replaces it; the refresh already on the panel is never
  cut short. In practice only `WORKING` is ever superseded, and that is the
  point: on a short hold with a quick backend the answer lands while the
  Listening refresh is still running, and `WORKING` would cost 889 ms to show a
  word that was stale before it appeared.
- **Nothing sleeps with the panel mid-refresh.** `finish()` waits for the task
  to go idle before `deepSleep()`, bounded like the capture join and for the same
  reason. That wait is past the answer chirp, so it is awake time rather than
  wait, and the log keeps the two apart.
- **The panel's numbers come from the task's clock.** After this step
  `screen.listening()` and the rest return at once, and timing them here would
  time a queue post -- a number about the poster rather than the panel, which is
  the trap S7b and S8 each fell into once. The log also wants two moments where
  it has one now: the release to the answer chirp, which this step shortens, and
  the release to the answer on the glass, which it shortens less, because the
  screens of one question still queue on one controller ([E8](experiments.md)).
- **A panel that will not start still has to reach the orchestrator.** It is the
  one failure with nothing to draw on, and the vision's answer -- chirp, log,
  sleep -- is the orchestrator's to give. Whether `begin()` stays here, ahead of
  the task, or the task reports it back is for the step to settle.

Once it has run, [D6](deferred.md) is paid and its line comes out of the
deferred list. [E8](experiments.md)'s second lever -- the power-down between two
refreshes of one question -- is worth another look then, since the panel's own
timeline is all that will be left of the panel in a question. It is not part of
this step.

**Verified by** S8's log over the same holds, short and long, with no panel left
in the release-to-chirp wait; a short hold, which is the superseded-`WORKING`
case by itself; the capture task's slow-chunk count unchanged with the display
task running; and the panel checked by eye through a run of consecutive
questions, which means asking.

### What it turned out to involve

`src/display.h/.cpp`. `Display` is `start()`, the five posts -- `clear()`,
`listening()`, `working()`, `answer()`, `error()` -- and `waitIdle()`, and it
owns the only `StickyScreen` there is, so nothing outside it can reach the panel
at all. The orchestrator's half of it is the calls it already made, minus the
waiting:

```
if (!display.start()) ...                 // the panel up, then the task
if (!stickyPower::wokeFromDeepSleep()) display.clear();
display.listening();
...                                       // the release, seen when it happens
display.working();
stickyBuzzer::taken();
...                                       // the round trip
display.answer(backend.answer());
stickyBuzzer::answer();
```

**The slot is one screen with replacement, and the pre-clear is not a screen.**
A post while another screen is still waiting replaces it and marks it
superseded; the task takes whatever is in the slot when the panel comes free.
The first sketch put `clear()` in the same slot, and on a cold start the
`listening()` posted straight after it would have replaced it before the task
had looked -- the one post that must never be dropped, dropped by the rule
written for the others. So the pre-clear is a flag the task honours ahead of
the slot. What can then be superseded is `WORKING` on any short hold, and
`LISTENING` on a cold start whose release comes before the pre-clear has
finished; both are right, because `StickyScreen::working()` repaints the band
over a frame buffer that is either `LISTENING` or white, and either is what the
glass holds.

**`begin()` stayed on the orchestrator's thread**, inside `Display::start()`,
before the task exists. The panel that will not start is then a return value
and the vision's chirp, log and sleep stay exactly where they were. It turned
out to be 201 ms rather than the milliseconds it looked like on paper -- E8 timed
the controller's re-initialisation inside a refresh at 24 ms, and the rest has
not been taken apart -- but it costs nothing the user waits for: `LISTENING`
cannot start before it on any thread, and until the release this thread has
nothing to do but watch.

**Core 0, at priority 1.** The step left the core to the device, against the
capture task's slow-chunk count, and the count did not move -- so the choice
rests on the argument, and core 1 was not tried. Core 1 is capture's and the
orchestrator's, and after [S11](#s11----streaming-upload) the orchestrator
streams the upload while the button is held; at priority 1 on that core the
panel would time-slice with it against half a second of drawing and plane push
at the very start of the stream. On core 0 everything else outranks it and
sleeps most of the time. The one thing it can hold up there is the idle task the
watchdog watches, and its longest run without blocking is about half a second
against a 5 s timeout.

**Every chirp now follows its post.** A post costs nothing and the chirps are
60, 230 and 450 ms of blocking, so the panel starts on the screen while the
chirp sounds instead of after it. The answer still appears a second after its
chirp, which is the order [S7](#s7----upload-and-answer) put the chirp in front
of the refresh for.

**Nothing prints between the release and the last chirp.** With the panel gone,
Serial1 was the next thing in the wait: 115200 baud is a millisecond every
eleven characters, arduino-esp32 gives `Serial1` no transmit buffer beyond the
UART's 128-byte FIFO, and the five lines that used to go out between the
release and the upload come to about 30 ms at that rate. They are written after
the last chirp now, in the order things happened, and the failure paths still
print as they go.

**The exit waits for the panel, and says for how long.** `finish()` shuts the
microphone and the radio down, then waits for the task to go idle, bounded at
`kDisplayDrainMs` for the capture join's reason -- the library waits up to 30 s
on a BUSY pin that never drops. That wait is past the last chirp and is printed
apart from the wait before it.

**The panel's numbers are the task's.** Each screen gets a record -- posted,
taken, finished, partial, superseded -- stamped on the task with
`esp_timer_get_time()`, so they share an axis with everything the orchestrator
measures, and they are read back once the task is idle. The log prints them as
a timeline from the top of `setup()`, which is also the axis the hold is on.

**[D6](deferred.md) is paid, and its line is out of the deferred list.** What
in it was a decision rather than a shortcut -- why `LISTENING` stays full, the
ghosting depth fixed at two, what an idle screen would cost -- has moved into
the vision's screen section, which is where the reasons for the panel's shape
now live.

### What it measured

Twelve wakes: a cold start that the flash turned into a tap, then eleven real
questions, the first of them paying for DHCP because the flash cleared RTC
memory. The panel was checked by eye throughout: `LISTENING`, `WORKING`, the
answer on every long and medium hold, `WORKING` never appearing on a short one,
and every transition clean.

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

- **There is no panel left in the wait.** Release to the answer chirp is the
  round trip plus 113 to 119 ms on every question, whatever the hold: 49-55 ms
  for the capture task to confirm the release (the 40 ms debounce, the chunk it
  lands in and this thread's 10 ms poll), 60 ms of taken chirp, 0 ms waiting for
  the network and 4 ms of everything else. [S8](#s8----the-flow) measured the
  same wait at 2.0 to 5.6 s with up to 2.1 s of leftover `LISTENING` and 990 ms
  of `WORKING` inside it; this run is 318 ms to 1.4 s, and the 1.4 s is a
  1277 ms round trip. It was audible at the desk before it was read off the log:
  the answer's pair now follows the taken note by about the round trip.
- **The short hold is the superseded-`WORKING` case, every time.** All four
  holds under 1.2 s had their answer in hand while `LISTENING` still had 1.1 to
  1.4 s to run, and `WORKING` was dropped unseen in all four. The answer went on
  the glass the moment `LISTENING` let go, at 2700 ms into `setup()` whatever
  the hold -- which makes those four questions 3997 ms awake to the millisecond.
- **The answer on the glass is shortened less, and the table says exactly
  why.** Four long holds came out at 2200 to 2204 ms from release to glass with
  round trips from 391 to 792 ms: the answer queued behind `WORKING`, which
  starts about 50 ms after the release and takes 895, so the glass is `WORKING`
  plus the answer's own 1250 whenever the round trip is shorter than `WORKING`.
  The one round trip longer than that, 1277 ms, queued for nothing and landed at
  2649. On a short hold it is `LISTENING` the answer waits for instead. That is
  [E8](experiments.md)'s point arriving in the firmware's log: the screens of
  one question queue on one controller, and the 338 ms each refresh spends after
  its image is drawn is now the panel's own business and nobody else's.
- **The recording did not notice.** Slow reads one in 14.2 to 15.5 across all
  eleven -- 20 of 295, 22 of 335, 50 of 745, 4 of 56 -- which is the DMA's
  240-frame blocks under 256-sample reads, exactly as [S4](#s4----capture-task)
  found; the longest read 30.1 ms, at the same chunk every time; and the task's
  clock within -6 to +4 ms of the audio it kept, which is less than a chunk.
- **The panel is as deterministic as ever**: `LISTENING` 2303-2307 ms full,
  starting 393-395 ms into `setup()` behind the 201 ms `begin()`; `WORKING`
  891-912 ms partial; the answer 1242-1256 ms partial. The answer's spread is
  one thing: 1242-1243 when it started behind `LISTENING`, 1252-1256 when it
  started straight behind `WORKING`, and the second is also when `finish()` was
  shutting the radio down on the same core, partway into the answer's plane
  push. Ten milliseconds of awake time, and the cost of core 0 if it is one.
- **The display task used 1.8 to 2.0 KB of stack.** It keeps the 8 KB it had
  as loopTask: the error screen and the library's fallback paths have not run
  on it yet, and the log keeps reporting the number.
- **The exit waits 1.0 to 2.4 s for the panel**, all of it past the last chirp.
  A long hold is awake for its hold plus about 2.25 s and a short one for about
  4 s from the wake.

**What this leaves for [S11](#s11----streaming-upload).** The wait after the
release is now the round trip and 115 ms that is not network. Of that, the
60 ms taken chirp blocks this thread, which is where the streamed tail will be
written from, so it wants to follow the terminating chunk rather than precede
it; the 50 ms of release confirmation is the debounce and stays.

## S11 -- Streaming upload

[D4](deferred.md) itself: the request opens while the button is still held and
the body goes up as it is captured, so what is left after the release is the
last few chunks of audio and the round trip. D4 has the argument, and
[E7](experiments.md) the reason it is worth more than the bytes -- the stalls
that cost one upload in four a whole retransmission timeout are spread through
the body, and they move under the hold with it.

- **`esp_http_client` replaces `HTTPClient`**, as the vision's transport section
  has said all along: Arduino's client sends only bodies of known length.
  `esp_http_client_open()` with a negative length announces a chunked body;
  whether `esp_http_client_write()` then frames the chunks, or leaves the
  `<size>\r\n...\r\n` framing and the terminating `0\r\n\r\n` to the caller, is
  the first thing to establish, against the backend, before anything is built on
  it. `Backend`'s four results and `unreachable()` get mapped again with it:
  today they lean on `HTTPClient`'s error codes, and on its habit of calling
  every failed connect "connection refused".
- **The body stays a WAV, with `0xFFFFFFFF` in both length fields.** Settled
  before the step; D4 has why. The vision's request contract says so when this
  lands.
- **The backend's half is its own.** The prototype in `assistant-server`
  rewrites the two fields once the terminating chunk is in, so the file it saves
  has a true length; the real backend will forward the stream to an external API
  and take the length from the transport. A stream that dies mid-question leaves
  the prototype a truncated file, and that is accepted -- the real backend will
  not keep recordings.
- **The buffer is shared for the first time.** Until now `Recording` changes
  hands on the release and nothing is locked. From this step the orchestrator
  reads samples the capture task is still writing past, so the committed count
  has to be published with the ordering that makes everything below it safe to
  read. The samples behind the count never move, which is what keeps it a
  counter rather than a lock -- and the paragraphs on ownership in `capture.h`
  and `recording.h` get rewritten rather than left describing a handover that no
  longer happens.
- **The request opens once the hold passes `config::kButtonMinHoldMs`**, or once
  the network is up if that is later. A tap has to reach nothing -- that is what
  the minimum hold is for -- and on a cached wake the network is usable about
  300 ms after the wake ([S7b](#s7b----cached-dhcp-lease)), which is where the
  minimum hold ends anyway.
- **The upload runs on the orchestrator.** After S10 it has nothing else to do
  but poll, and it already owns WiFi, the error table and
  `askRenewingStaleLease()`. A write that blocks through one of E7's stalls
  blocks only this thread: the release is timestamped by the capture task, and
  the buffer is linear, so there is nothing to overrun.
- **A retry starts again from byte zero.** The buffer holds the whole recording,
  so the stale-lease rule in `askRenewingStaleLease()` keeps its shape; what
  changes is that a retry can now happen while the user is still talking, and is
  a stream in its own right. What a server that fails outright during the hold
  does to the recording -- the vision aborts on a WiFi failure, and the same
  argument applies -- is for the step to settle.
- **`config::kResponseTimeoutMs` counts from the terminating chunk.** It is the
  backend's thinking time, and a 30 s hold must not spend it.
- **The round trip in the log splits in two**: the tail after the release, which
  is what streaming exists to shrink, and the answer after the terminating chunk,
  which is the number [E2](experiments.md) will want.

**Verified by** the release-to-chirp wait against S10's over the same holds --
the tail after the release should be the last chunks of audio plus the round
trip, with E7's stalls landing under the hold on long questions and out of the
wait; the byte count in the answer matching the recording; the saved file
playing back as the question; a tap reaching nothing; and the stale-lease rule
walked once more, since a retry now restarts a stream -- which needs a way to
provoke it again, the seam S7b used having gone with its driver.

### What it turned out to involve

`src/backend.h/.cpp` is a new client on `esp_http_client`, and the request is
four calls where it was one: `open()` while the button is held, `write()` as the
capture task commits, `end()` for the terminating chunk, `receive()` for the
answer. The orchestrator's half:

```
while (!capture.finished()) {                 // the hold
  if (wifi.poll() == Failed) capture.abort();
  else if (wifi.online() && capture.pastMinimumHold()) stream();  // open, then send what is new
}
display.working();
const bool streamed = backend.streaming();
if (streamed) endBody();                      // the tail and the terminating chunk
stickyBuzzer::taken();
if (!streamed) { /* network, open, the whole body */ }
conclude(backend.receive());
```

**`esp_http_client_write()` does not frame chunks.** Settled from the IDF
source the prebuilt libraries were built from -- `release/v5.5` at `87912cd291`,
which is `v5.5.2` plus `esp_http_client_get_socket()`: `esp_http_client_open()`
with a negative length writes `Transfer-Encoding: chunked` instead of a
`Content-Length` and does nothing else, and `write()` is `esp_transport_write()`
in a loop. So the size line, the CRLF and the terminating `0\r\n\r\n` are
written here, and the device confirmed it: every answer's byte count matched
the recording to the byte, which a body framed twice could not have done.

**Each chunk is one write, with Nagle off.** A chunk framed as three writes would
send the samples straight from PSRAM, but with Nagle's algorithm on, the
terminating chunk -- five bytes -- waits for the backend to acknowledge whatever
is in flight, and a backend that delays its ACKs puts that delay on the one
write whose latency is the whole wait. With it off, three writes would be three
packets. So each chunk is framed into a 2 KB buffer in the object and goes out
whole, and `TCP_NODELAY` is set through `esp_http_client_get_socket()`. A chunk
under the hold is what the capture task committed since the last pass of the
loop, 512 bytes most of the time; the 2 KB only bounds the backlog.

**The four results are mapped on esp-tls's own verdict, and the clock is
gone.** HTTPClient called every failed connect "connection refused" and S7 had
to read the elapsed time to tell a refusal from silence. esp-tls keeps the two
apart: a connect that runs out of its budget is
`ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT`, which is `unreachable()` and the shape
of a stale lease, and a closed port is a failed connect with errno 104 behind
it -- `ECONNRESET`, which is what lwIP makes of the RST. A write that fails, or
that takes nothing for the stall budget below, is `NO SERVER`; headers that do
not come within the response budget are `TIMED OUT`, and so is a reply that
stops short; a reply over 4 KB is `BAD RESPONSE`.

**There is a third timeout.** A write that sits through one of E7's stalls needs
a budget of its own: longer than the connect's 5 s, because a connection that
has answered is one that works, and the longest stall E7 measured on a working
one was 4243 ms, with a second loss behind its slowest first retransmit coming
to 7.6 s. `config::kBackendStallTimeoutMs` is 10 s. Under the hold a stall costs
nothing, so the only price of a long budget is a dead backend noticed later.
`config::kResponseTimeoutMs` now counts from the terminating chunk, as the step
asked: `receive()` sets what is left of it, because the taken chirp stands
between the two.

**The request waits for `Capture::pastMinimumHold()`, not for the clock.** The
first reading of "once the hold passes the minimum" was the clock, and it is
wrong by a debounce window: a release 20 ms before the minimum is not confirmed
until 20 ms after it, and a request opened in between is a tap reaching the
backend. `StickyButton` now latches the moment `poll()` sees the line down at
the minimum or later -- after which the press cannot turn out to be a tap -- and
the capture task publishes it.

**The buffer is shared through one count.** `Recording`'s sample count is an
atomic that `commit()` stores with release ordering and every reader loads with
acquire, so what the orchestrator reads below it has landed, and nothing above
it is read. The header is written once, by `begin()`, with `0xFFFFFFFF` in both
length fields, so the buffer is a complete WAV at every length and `wav()` is
`const`. The ownership paragraphs in `capture.h` and `recording.h` say this
now: the microphone and the button are handed over, the buffer is shared.

**The taken chirp follows the terminating chunk**, as S10 left it wanting,
when the request was open at the release -- the tail is two or three
milliseconds, and the 60 ms of chirp is then the backend's to think in. When the
release comes first, the chirp comes first: the question is then the pre-S11
one -- wait for the network, open, send the whole body -- and a chirp that
waited for all of that would say nothing about the button.

**A failure under the hold ends the question there**, which is what the step
left to settle, and the vision's WiFi argument decided it: the error screen and
the chirp come while the button is still down, and `conclude()` stops the
recording first. The vision's error section says so. The stale-lease rule
retries only the open -- a connection that answered and then died is not the
lease -- and every retry is a new request from the header, under the recording.

**A request that opened after the release** is timed in the log as the tail,
connect and body together, so the parts still add up to the wait.

**Two readings in the log are the instrument's, not the backend's.** The
answer's first byte is read after the taken chirp, so a backend that answers in
less than 60 ms reads as 62 ms, which is every question below; E2 now says so.
And the longest write is timed around `esp_http_client_write()`, which blocks
until the socket has taken the bytes, not until they have arrived.

**The prototype backend fills in the lengths.** `assistant-server` rewrites the
two fields once the stream is in, only where they say `0xFFFFFFFF` and walking
the chunks to find `data`; a stream that dies keeps what arrived, with its
lengths filled in. It also logs that a body came chunked and how long it took to
arrive. Checked against a second instance on the laptop with a chunked request
and with one cut off halfway. The server on the desk was not restarted during
the runs below, so their files kept `0xFFFFFFFF` -- which showed on the way that
the old prototype took the chunked body without a change -- and copies of
them were given their lengths by the same function. Restarted afterwards, it
filled them in on a question from the device: 286252 bytes, 8.94 s, both fields
right.

**The E7 rig builds against the new `Backend`**, sending its payload as a body
of one write. Its result in [experiments.md](experiments.md) went through
HTTPClient, and the rig says so.

**The error table was walked by a driver that was not committed**: a table of
URLs through `Backend::open(url)`, one row per question, and S7b's
`spoilLease()` restored from history for one row. `Backend::open(url)` stays as
the seam, as `ask(url, ...)` did.

[D4](deferred.md) is paid, and its line is out of the deferred list. What in it
was a decision rather than a shortcut -- the body staying a WAV, and why
`0xFFFFFFFF` -- has moved into the vision's request contract.

### What it measured

Ten questions and two taps, then the error table. The panel was checked by eye
throughout, and every transition was what S10 draws.

| Hold | Request opened | Longest write | Release to the answer chirp | `WORKING` | Release to the answer on the glass |
| --- | --- | --- | --- | --- | --- |
| 3927 ms, DHCP | 3447 ms | 13 ms | 117 ms | drawn | 2199 ms |
| 3402 ms | 400 ms | 8 ms | 122 ms | drawn | 2201 ms |
| 4722 ms | 397 ms | 306 ms | 122 ms | drawn | 2201 ms |
| 10722 ms | 395 ms | 4 ms | 116 ms | drawn | 2198 ms |
| 2187 ms | 397 ms | 309 ms | 117 ms | superseded | 1830 ms |
| 2367 ms | 397 ms | 5 ms | 121 ms | superseded | 1684 ms |
| 717 ms | 397 ms | 307 ms | 154 ms | superseded | 3281 ms |
| 747 ms | 396 ms | 8 ms | 118 ms | superseded | 3279 ms |
| 912 ms | 400 ms | 6 ms | 123 ms | superseded | 3135 ms |
| 1497 ms | 397 ms | 309 ms | 121 ms | superseded | 2520 ms |

- **The round trip is out of the wait.** Release to the answer chirp is 116 to
  123 ms on nine questions of ten and 154 on the tenth, whatever the hold:
  47-55 ms for the release to be confirmed, 2 ms of tail, 60 ms of taken chirp,
  3-4 ms waiting for an answer that had arrived during the chirp, and 2-3 ms of
  everything else. [S10](#s10----display-task) measured the same wait at 318 ms
  to 1.4 s, the round trip plus 115; what is left is the debounce and the chirp.
- **Even the DHCP wake streamed everything under the hold.** Its address came
  at 3436 ms and the request opened 11 ms later; by the release at 3927 the
  3.4 s of backlog had gone up, and the tail was 2 ms.
- **The body went up whole every time.** The answer's byte count matched the
  recording on every question, 23084 to 866348 bytes, and so did the size of
  the file the backend saved. Neither tap reached the backend: no request was
  opened for either.
- **No stall reached a question.** 2.5 MB went up across this run and the one
  below, and no write took longer than 316 ms -- where E7's rate would have put
  about five stalls of a second or more into that much body. Written up under
  [E7](experiments.md), with the likely reason and without the capture that
  would prove it.
- **The first write after the connect sometimes takes 300 ms**, 303 to 316 ms
  in six of the fifteen requests that connected and 4 to 22 ms otherwise, always
  the write that carries the backlog at about 450 to 550 ms into the wake. The
  cause is not established. It sits under the hold and costs nothing, except when
  the release lands inside it: the 717 ms hold did, so this thread heard of the
  release 70 ms after it rather than 50, and had 17 ms of tail to send -- the
  154 ms above.
- **LISTENING is 50 to 105 ms longer while a stream runs under it**: 2359 to
  2412 ms on every question whose request opened at 400 ms, against 2305 to
  2312 on the taps, the DHCP wake and the walk's rows whose requests did not
  connect until after it. The display task is on core 0 at priority 1, where
  lwIP handles the stream's packets, which is the likely reason. What it costs is
  the answer on the glass on a short hold, which waits for LISTENING: it ends at
  2754 to 2807 ms into the wake now against 2700 at S10.
- **`WORKING` is dropped on every hold that ends inside LISTENING**, which is
  any shorter than about 2.6 s, not only the short ones: the answer arrives about
  120 ms after the release, while LISTENING is still refreshing. S10 drew it late on the 2.3-2.5 s holds; here the answer went
  straight on after LISTENING, 1684 and 1830 ms after the release against S10's
  2361 and 2533.
- **The recording did not notice the stream on its core**: slow reads one in
  14.3 to 15.7 over every question of both runs (S10: 14.2 to 15.5), the
  longest read 30.1 ms at chunk 6 every time, and the task's clock within 7 ms of
  the audio it kept.
- **The display task used 1.7 to 2.2 KB of stack**, and the error screen has now
  been drawn through it.

The error table, one row per question:

| Row | Hold | Screen | When |
| --- | --- | --- | --- |
| a closed port | 3961 ms | `NO SERVER` | 63 ms after the open, at 3.5 s -- under the hold |
| an address nothing answers at, on a lease | 3327 ms | `NO SERVER` | 18.7 s into the wake |
| `/audio/fault/500` | 2982 ms | `SERVER ERROR 500` | 138 ms after the release |
| `/audio/fault/empty` | 3207 ms | `BAD RESPONSE` | 119 ms after the release |
| `/audio/fault/slow` | 10302 ms | `TIMED OUT` | 30002 ms after the terminating chunk |
| a lease spoiled onto another subnet | 27072 ms | the answer | 122 ms after the release |
| the endpoint | 3792 ms | the answer | 115 ms after the release |

- **A refusal ends the question under the hold, and leaves the lease.** The
  closed port refused in 63 ms on a DHCP wake whose request opened at 3447 ms;
  the recording was aborted at 3.47 s of audio with the button still down, the
  error came, and the device waited for the release before sleeping. The next
  wake found the lease the DHCP had just granted.
- **The stale-lease rule runs under the hold, and a retry restarts the
  stream.** On the spoiled lease two connects answered nothing in 5007 ms each,
  DHCP took 3211 ms and handed back the real address, and the request reopened
  at 13.6 s into a 27 s hold and streamed from the header: 866348 bytes, the
  whole recording, answered 122 ms after the release. S7b measured the same
  failure at 13.2 s of wait after the release; here none of it was.
- **The rule still costs a false positive what it did.** The address nothing
  answers at, on a good lease, took 5007 + 5000 ms of connects, 3230 ms of DHCP
  that handed back the same address, and one more 5000 ms connect -- the error
  at 18.7 s, 15.3 s after the release.
- **The response budget counts from the terminating chunk**: `TIMED OUT` came
  30002 ms after it, on a 10.3 s hold. S7 measured 30.5 to 32.9 s from the start
  of the POST, with the upload inside the budget.
- The error screen is a 1317 to 1333 ms partial, against the answer's 1243.

**What this leaves.** After the release the wait is the debounce, the taken
chirp and the backend, and against a backend that answers a fixed phrase the
backend is nothing -- 115 to 154 ms in all. The two small findings above, the
300 ms first write and the longer LISTENING, are both under the hold on long
questions and cost the short ones at most a few tens of milliseconds on the
glass. The next number that matters is the backend's own, which needs a model
behind it; [E2](experiments.md) now says what streaming moved out of its way.

---

## S12 -- DNS server

The home router stops answering DNS now and then while it goes on routing
everything else. Phones do not notice, because they bring a resolver of their
own; the device looks the backend's host name up on every question, and a
lookup nobody answers ends that question with NO SERVER. `secrets::kDnsServer`
names a DNS server to use in place of the one DHCP hands out, and empty keeps
the network's.

### What it turned out to involve

`src/wifi_link.h/.cpp`, and a third argument to `WifiLink::begin()`, which
refuses anything that is not an IPv4 address the way it refuses a missing SSID.

**It cannot go in before the address.** lwIP's DHCP client hands the servers in
its ACK to `dns_setserver()`, up to `DNS_MAX_SERVERS` of them -- three in this
build -- over whatever was there, and with `LWIP_DNS_SETSERVER_WITH_NETIF` off
that is one table for the whole stack. A DNS argument to `WiFi.config()` ahead
of DHCP is exactly what gets overwritten. So the server goes in on the poll that
turns Online: DHCP, the cached lease and `renewAddress()` all end there, and
nothing can look a name up before it.

**In place of the network's, not ahead of it.** `WiFi.setDNS()` clears the
backup slot as well, so the server the log line names is the only one asked.

**The lease cache keeps the network's server.** `installDns()` runs after
`recordSuccess()`, so the entry holds what DHCP said, and emptying the setting
takes effect on the next wake rather than once the lease ages out.

The flow's `online in` line now names the DNS server and where it came from, read
back from the stack rather than from the setting, so a replacement that did not
take shows up as the network's.

---

## S13 -- Fonts

The panel draws Latin, Greek, Cyrillic and the punctuation the latter two
bring. `translit.py` is gone from the backend, and with it
[D1](deferred.md) and the last place where the server knew what the device
could draw.

### What it turned out to involve

`tools/gfxfont.py`, `src/fonts/`, `src/text.h/.cpp`, `src/sticky/screen.*`,
`src/display.cpp`, and on the server side `translit.py` deleted and one call
in `main.py`.

**`SmoothFont` was the plan and was wrong.** D1 recorded VLW as the path,
because it looks glyphs up by code point. It renders with alpha blending, and
`Panel_EPaper::writePixel` on a 1 bpp panel is `if (color) white else black` --
only pure `0x0000` is ink. Every blended edge pixel goes white, so a VLW glyph
arrives as its fully-opaque interior and nothing else: eight bits of alpha per
pixel paid for, one bit used. Against that, a GFXfont is already one bit, and
`first`/`last` are `uint16_t`, so the format reaches Cyrillic on its own.

**What blocked GFXfont was that a range cannot have holes.** Latin ends at
0x7E, Cyrillic starts at 0x401 and the numero sign is alone at 0x2116; one
range spanning them is 8455 descriptors, 67 KiB per face, for 250 glyphs. So a
face is several GFXfonts, one per script, and `src/text.cpp` picks between
them per character -- 2.3 KiB of descriptors per face, no holes, and every
code point at its own value. Greek cost nothing structural on top of that: it
sits at 0x370-0x3FF, immediately before Cyrillic.

**Owning the layout was not optional anyway.** `Seeed_GFX2::textWidth()` reads
bytes rather than characters, so any non-Latin string measures as empty and
`drawString()` centres it wrongly; neither is virtual. Measuring and placing
had to move into the firmware regardless, and once they had, several fonts per
face cost one comparison per character. The same loop is what
[S14](#s14----word-wrap) went on to break into lines.

**The generator reproduces the bundled headers, nearly.** Regenerating
0x20-0x7E from the same TTFs at fontconvert's 141 dpi gives a glyph table
identical to Seeed_GFX2's, byte for byte, and bitmaps that differ in about 50
bytes of 7464 -- single edge pixels, from a FreeType nine years newer. That is
why all three faces are generated rather than only the new scripts: one
rasteriser per string.

**The box grew, and the band with it.** `ascent` is now 41 px rather than the
Latin 34, because Ё's diaeresis clears the cap height, so an all-Latin word
sits a few pixels lower than it did. The alternative -- centring on the ink of
the actual string -- would make LISTENING and WORKING centre differently and
break the one thing `wordBand()` guarantees, which is that the band is a
property of the face. Checked on the host: with Ё in it, a word's ink reaches
12 px from the band's edge, which is exactly `kWordBandMargin`.

**Cost.** 44.4 KiB of flash, 1219520 to 1264991 bytes, against 3 MiB of app
partition. No RAM: the faces are `const` and read from flash in place.

### Checked on the device

Everything here was first verified by compiling `src/text.cpp` and `src/fonts/`
against a host stub of `Seeed_GFX` and rendering the screens to an image:
placement, script switching mid-string, the `?` fallback, the band, and that
`textCopy()` cuts between characters. What that could not show was the glass --
whether 24 pt Cyrillic at 235 dpi is as readable as the Latin was, and whether
the partial refresh of the word band still lands cleanly now that the band is
130 px rather than 136.

Both were answered on 2026-09-20, on the run that checked
[S14](#s14----word-wrap): Cyrillic reads, and the band's partial refresh took
891 to 893 ms over four consecutive questions with none refused and nothing
smeared between `LISTENING` and `WORKING`.

---

## S14 -- Word wrap

An answer is laid out as lines now, inside a box with a 40 px margin on all
four sides, and the block of them is centred in it. An answer with more in it
than the box has room for is drawn to the last line that fits and that line
ends in an ellipsis. Half of [D2](deferred.md) is gone; what is left there is
reaching the rest of a long answer.

### What it turned out to involve

`src/text.h/.cpp`, `src/sticky/screen.*`, the two buffer bounds in
`src/display.h` and `src/backend.h`, and `tools/preview/`.

**The hard part had been done at [S13](#s13----fonts).** Wrapping needs the
width of a string one character at a time, and that is exactly what owning the
layout bought: the loop that picks a font per code point already had the
advance in its hand. What was left was to decide where to stop.

**Measuring and drawing were made one thing rather than two similar things.**
`textWidth()` and `textDraw()` had a copy each of the per-character advance;
now both go through `advanceOf()`, and drawing goes through a `drawRun()` that
takes a run of bytes rather than a terminated string, because a wrapped line is
a run of the answer rather than a copy of part of it. A line measured one way
and drawn another would show up as an ellipsis in the margin.

**Where a line ends**: at the last space that fits, at a newline in the text,
or -- when a single word is wider than the whole line -- inside the word, after
the last character that fits. That last case is not only for the 60-character
German noun: it is what makes the loop terminate at all, since a line that
cannot end is a line that is laid out forever. The spaces a line breaks at are
drawn at neither end of it.

**The ellipsis is laid out, not appended.** When the last line the panel has
room for is not the last line of the answer, that line is laid out a second
time against a width short by the ellipsis, and the ellipsis is drawn after it.
Appending to a line that already filled the box would put it past the margin;
reserving the room on every line would narrow the six that did not need it.

**The block is centred as a block**, so a one-line answer sits exactly where a
one-line answer sat before this step, and a five-line one grows evenly around
that point rather than dropping down the panel.

**The buffers grew, and that is the part of this with numbers in it.** Under
D2 the bound on an answer was a line's worth; now it is a panel's worth, so
`StickyScreen::kMaxTextChars` went from 128 bytes to 640 and
`Backend::kMaxAnswerChars` from 256 to 768. Measured on the host in
`tools/preview`: the box is 720 x 400 and holds seven lines of 24 pt; filling
all seven takes 375 bytes of a real Russian sentence, or 560 bytes of the
narrowest Cyrillic letter repeated with no spaces to break on. So 640 is past
anything the panel can show and what `textCopy()` drops is text the layout
would have dropped too -- the difference being that running out of lines leaves
an ellipsis and running out of buffer does not. The one string that beats it is
a panel of the narrowest three-byte punctuation, 1260 bytes, and a sentence
made of quotation marks is not an answer.

The backend's bound stays the looser of the two on purpose. Its cut is
`snprintf()`'s and lands on a byte, so it can halve a character; the screen's
is `textCopy()` and lands between characters. Keeping the screen's the tighter
one means the half character never reaches the panel.

The error screen was left alone. Its title comes from the vision's table and
its detail is an HTTP status code, so the detail got a bound of its own --
64 bytes -- rather than the answer's, because both are copied onto a task's
stack in `Display::post()`.

**Cost.** 1120 bytes of flash, 1264995 to 1266115, and 960 bytes of static
RAM, 55796 to 56756 -- the two buffers that grew, both inside objects that are
globals in `main.cpp`. The `Slot` the display task takes a copy of is about
700 bytes instead of 260, on an 8 KB stack that was using 2.0 of it.

### What it measured

The host preview draws the wrapping against the real font data, so the
breaking, the centring, the ellipsis and the box were checked there first --
see `tools/preview/README.md` for what that cannot say.

**On the device, 2026-09-20**, four questions in a row, whose answers came to
2, 2, 2 and 3 lines. The wrapping, the left margin and the block centred in the
box are right on the glass, and the two numbers the run was for:

- **The answer's partial refresh was 1259 to 1273 ms**, against the 1344 ms the
  vision quotes for a whole-panel partial. The spread across those four answers
  is 14 ms and it tracks the ink, so a wrapped answer costs no more to put on
  the glass than the one-line answer did -- [E8](experiments.md)'s finding that
  the cost is waveform rather than transfer, arrived at from the other side.
- **The display task had 5836 to 5868 of its 8192 bytes of stack never used**,
  where [S10](#s10----display-task) measured 1.8 to 2.0 KB in use before the
  slot grew. So the extra 450 bytes of slot show up about where they should and
  the margin is still most of the stack.

**What has not been on the glass is the case with the ellipsis in it.** The
model is asked for a sentence or two and gives them: the longest of those four
answers was 151 bytes and three lines, and nothing the assistant says in normal
use comes near the 375 that fills the panel. So the overflow path -- which is
both what [D2](deferred.md) is now about and the most ink a partial refresh
here will ever have to drive -- has run only in the host preview. Provoking it
on the device needs a backend that answers with something long, which is a URL
in `src/secrets.h` and a reflash.


---

## Settled while planning

Six questions came out of writing this down. All six are answered; the
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

  *Reopened and closed by [S14](#s14----word-wrap), 2026-09-20.* There is a model
  behind it now, and it answers in sentences. Half of D2 went with it; what is
  left is the answer too long for a whole panel.
- **How long the upload takes does not matter yet.** Nothing waits on the
  answer, so the number has nowhere to land until [D4](deferred.md) is being
  weighed for real. (S7)

  *Reopened and closed by [E7](experiments.md), 2026-09-17.* It is being weighed
  for real now: the upload's stalls are spread through the body, which is the
  case where streaming moves them under the hold. D4 has the argument.
- **The streaming body stays a WAV**, with `0xFFFFFFFF` in both length fields,
  rather than becoming raw PCM with the format in request headers -- the
  vision's request contract has why. (S11)
- **A stream that dies leaves the prototype backend a truncated file**, and that
  is accepted: the real backend will forward recordings to an external API
  rather than keep them. (S11)


## Battery indicator — 2026-09-21

Added a battery outline with proportional fill and a percentage in the upper-left
margin on Listening, answer and error. Working retains it during its middle-strip
refresh, and e-paper retains the last reading in sleep. The existing answer box
is unchanged. BQ27220 reads run only on the display task, with a 20 ms I2C timeout;
missing/invalid readings show `?`. Serial screen records include the reading.
No gauge configuration is written. Firmware built and flashed to the connected
E1005; Serial1 confirmed `battery 100%` on Listening after reset. The host preview
was inspected at 100%, 50%, 1%, 0% and unavailable, including a seven-line answer.
Gauge accuracy over a discharge cycle remains to be measured.


The user confirmed that the battery indicator is visible and clearly readable
on the device. Reduced its percentage label from FreeSans 18 pt to 12 pt on
request, keeping the icon geometry. Added a generated 12 pt face and regenerated
the existing faces together. The updated firmware was built and flashed; the
user confirmed that the smaller label looks better and approved the final layout.

## Silent mode — 2026-09-21

For evening development, Up (GPIO5, next to AI) wakes the device and toggles a
persistent silent flag. NVS/Preferences stores it across deep sleep and complete
power loss; write only on a change. Default is sound enabled. Load before any
buzzer call; mute every pattern, including errors. Toggling itself never chirps.
The Up-only wake path starts neither microphone nor WiFi. Up is ignored during
an ordinary question; simultaneous AI/Up wake gives Up priority. Before sleep,
require both wake buttons to be stably released, without autorepeat.

Reserve a white rectangle in the top margin centred at x=200 (25% of 800).
Draw a crossed-out speaker only when silent, on every normal screen. Up updates
only that rectangle and preserves the last answer. Reconstruct its previous
pixels from the last successfully displayed flag, then send old/new planes for
a partial refresh. No full-screen snapshot is needed. Track display validity
in RTC memory; cold boot, failed or interrupted refresh makes it unknown.
Unknown state requires a full reconciliation (a cleared screen with indicators
on the toggle path); normal cold startup already clears before Listening.
Wait for refresh completion and stable release before deep sleep.

Hardware acceptance: both toggle directions, holding/bouncing Up, consecutive
toggles without an AI cycle, unchanged answer outside the indicator, no sounds
including errors, AI cycle while muted, Up ignored during a question, persistence
through power loss, and recovery after an interrupted refresh. Icon placement
and partial-refresh quality require the user's visual check. Without a ready
chirp, the user accepts a short pause before speaking. No hardware timing is
assumed. The feature's long-term role can be reconsidered after development.

Implementation follows the plan. `silent_mode` owns the NVS preference; the
buzzer checks it centrally. The normal startup reads it before microphone and
error paths. A failed preference open mutes conservatively; a failed write
leaves the current value unchanged and is logged. Up wake is enabled only by
the main firmware so the measurement rigs keep their existing wake sources.

The screen retains validity and the last displayed flag in RTC memory. A
shadow-priming pass runs the library's own coordinate and mirror transformations
but suppresses image transfer and the refresh waveform; the real partial then
sends both planes. It does not mark the rest of the screen as reconstructed.
The icon's shared drawing code is included in the host preview.

Validation so far: main firmware builds; host preview inspected beside the
battery and seven-line answer. A temporary host harness against the actual
`Driver_SSD1677_Sticky` header confirmed that priming sends no image and triggers
no waveform, and that the old/new inverted planes and next shadow match across
a multi-row window. This does not prove physical partial-refresh quality.
Hardware acceptance was pending at this stage; see the confirmation below.

All six PlatformIO environments built successfully: reterminal_e1005, exp_e1,
exp_e1_firmware, exp_e6, exp_e7 and exp_e8. The main firmware was then rebuilt
and uploaded to `/dev/cu.usbmodem5C843360331`; esptool verified the flash hash
and reset the board. A subsequent 12-second Serial1 read returned no bytes, so
it provides no runtime confirmation. User-operated Up/AI tests and visual
acceptance were still pending at this stage.

### First hardware feedback and controller-RAM fix

The user confirmed Up toggles the mode/indicator and suppresses the buzzer.
However, the first implementation corrupted the rest of the screen with noise
while updating the icon cleanly (photo dated 2026-09-21 13:45:03). The earlier
host harness verified only the bytes inside the window; it did not model the
uninitialized RAM elsewhere after EPD_EN was cut during deep sleep.

SSD1677 RAM X/Y windows (0x44/0x45) limit writes, not the display scan; see the
[controller datasheet, sections 8.3–8.4](https://cursedhardware.github.io/epd-driver-ic/SSD1677.pdf).
After shadow priming, the first real partial now seeds both entire controller
planes from the same reconstructed shadow, without a display activation.
Outside the icon both planes contain identical white bits; inside, the normal
old/new upload installs the intended transition. This removes random RAM
transitions without storing the last answer. The panel's differential waveform
leaves equal pairs unchanged, preserving existing dark text; the user confirmed
the corrected behavior on the device below. This is RAM initialization, not a full
optical refresh. The ordinary within-question partial path is unchanged.

Added a host regression (now `test/test_epaper/`, run via PlatformIO):
a fake controller starts both planes
with different garbage, then checks both toggle directions, multi-row windows
at the RAM boundaries and the indicator position, exact old/new polarity,
equality everywhere outside the window, no activation during priming, and the
next partial's shadow. The host regression passes. Physical acceptance of this
fix is recorded below.

The controller-RAM fix built successfully in all six environments and was
uploaded to the connected E1005 with flash hash verification. Test by obtaining
a fresh AI answer (full Listening refresh removes prior corruption), then
repeatedly toggling Up in both directions. Confirm that dark answer text and
the battery indicator stay unchanged, not merely that the noise disappears.
The user subsequently confirmed that everything works on the device after this
fix (2026-09-21). Silent-mode switching, buzzer suppression and preservation of
the surrounding screen during indicator updates are accepted. This completes
the feature's normal-use hardware verification. Power-loss persistence and
interrupted-refresh recovery are implemented but were not separately reported
as tested.


## Temperature and humidity indicator — 2026-09-21

Added SHT40 temperature and relative humidity in the upper-right margin,
right-aligned 16 px from the edge, using the battery label's 12 pt face and
vertical placement. Both values have one decimal place. Listening, Answer and
Error sample before drawing; Working retains the previous header, as does an
Up-only silent-icon partial update. The cold-start silent screen also samples.
Deep sleep keeps the last visible readings without periodic wakeups.

The battery gauge and SHT40 share a display-task-only I2C initializer (SDA 1,
SCL 0, 100 kHz, 20 ms timeout). SHT40 uses address 0x44 and command 0xFD,
waits 10 ms, validates both CRC-8 words, then converts per the Sensirion
SHT4x datasheet. No heater or calibration offset is applied. RH is clamped to
0–100%; failures show `--°C  --%`. Serial1 reports values beside battery state.

Host preview rendered normal, negative-temperature/100% RH and unavailable
labels; visual inspection confirmed they fit above even the seven-line answer
without overlapping the battery or silent icon. Sensor accuracy and physical
screen acceptance still require the device and comparison with a reference.

All six PlatformIO environments built successfully (reterminal_e1005, exp_e1,
exp_e1_firmware, exp_e6, exp_e7, exp_e8); `git diff --check` passed.

Uploaded the main firmware to `/dev/cu.usbmodem5C843360331`; esptool
verified the flash hash and reset the board. Visual confirmation and actual
sensor readings remain pending user operation of the AI button.


### Hardware feedback and bolder indicators

The user confirmed that temperature and humidity are visible and working on
the device. Accuracy comparison against another sensor is deferred until they
have collected observations over time.

At the user's request, removed the C after the degree sign and switched both
climate and battery labels to FreeSans Bold at the same 12 pt size. Added a
proper bold face to `tools/gfxfont.py` and regenerated all faces together;
existing generated faces were byte-for-byte unchanged. Host preview confirms
normal, boundary and unavailable values fit in the existing top margin.

All six PlatformIO environments built successfully. Uploaded the updated
main firmware to `/dev/cu.usbmodem5C843360331` with flash hash verification.
The user confirmed that the revised typography looks correct on the device.
Display behavior and typography are accepted; sensor accuracy comparison
against a reference remains pending longer-term observations.


## Neutral notebook screen — 2026-09-21

Startup without AI held and AI wakes released before setup now show a notebook
without starting capture, WiFi or the ready chirp. A tap discarded after capture
posts the same screen before finish() drains the display queue. Successful
questions retain the server answer. The normal battery, climate and silent-mode
header remains. Idle uses the existing whole-panel refresh path: full if no
frame has run this boot, partial after Listening or the cold-start pre-clear.
The notebook geometry is shared with the host preview in notebook_icon.h.

Host preview inspected: the notebook fits centrally and clears the header.
The user confirmed that the updated behavior works correctly on the device.

All six PlatformIO environments built successfully; git diff --check passed.
Uploaded reterminal_e1005 to /dev/cu.usbmodem5C843360331 with flash hash
verification. Serial cold-start check (POWERON, no button held) reports
pre-clear, notebook, then idle/sleep at 3959 ms, with no capture or WiFi path.
The notebook refresh retained valid battery and climate indicators. The user
subsequently confirmed that everything works correctly on the device; hardware
acceptance is complete.


### E1 firmware timer-wake compatibility

The startup idle check initially bypassed microphone/ready timestamps during
exp_e1_firmware's unattended timer phase, leaving every timed row excluded
from its summary. Added an experiment-only linker wrapper for the startup
StickyButton::isDown() call: cold/timer wakes enter capture, while real button
wakes retain their physical state. Button polling inside button.cpp is not
wrapped, so an unattended capture still ends as a discarded tap. No production
logic or experiment-specific conditional was added to main.cpp.

Validation: reterminal_e1005 and exp_e1_firmware both build successfully;
git diff --check passes. Inspected the linked experiment disassembly: setup()
calls the isDown wrapper, the wrapper calls the real method, and poll() still
calls the real method directly. The device is not currently connected, so
the corrected timer sequence has not yet been rerun on hardware.


## Host unit tests — 2026-09-21

Added a PlatformIO native/Unity environment compiling production text.cpp,
fonts and recording.cpp. Fourteen tests cover UTF-8 recovery and copy
boundaries, script support and fallback, wrapping and ellipsis, streaming WAV
headers, sample publication, capacity, duration, buffer reuse/reallocation,
allocation failure and cleanup. Graphics reuse the existing preview stub;
Recording substitutes only the ESP allocator with host malloc/free.

CI runs the native suite and the existing e-paper shadow RAM regression in a
separate test job. The build job still builds all embedded environments,
excluding native. Local commands and scope are in test/README.

Validation: all 14 native tests and the shadow RAM regression pass on the
host; git diff --check passes. Production firmware code is unchanged. No
hardware tests, flashing, timing validation or complex peripheral mocks were
added, per the requested scope. This does not establish hardware acceptance.


### E-paper regression in the native suite

Moved the Python-generated C++ e-paper regression to test/test_epaper/ and
Unity. Its fake controller/base-driver headers now live in test/support; the
allocator is shared with the Recording tests. The six original combinations
(on/off at the icon and both RAM boundaries) are individually reported test
cases. Both full RAM planes are checked after the subsequent partial as well
as the first one. Production driver behavior is unchanged.

Removed the Python runner and its separate CI command. All host checks now run
with `pio test -e native`; `-f test_epaper` selects the display suite alone.
Validation: all 20 native tests pass; git diff --check passes. No device needed.


## S15 -- Idle dashboard

**Implemented 2026-09-22; hardware acceptance pending.**
The shared scenario is in
[idle-screen.md](../../server/docs/use-cases/idle-screen.md); the wire format
and scheduling semantics are in
[device-contract.md](../../server/docs/device-contract.md#дашборд).
The device decisions are in [project-vision.md](project-vision.md#idle-dashboard).

### Build order

1. Add the authenticated dashboard endpoint on the backend with a diagnostic
   uncompressed frame and `Next-Update-After`. Establish bit order, polarity,
   orientation and telemetry using the shared contract before choosing widgets.
2. Add bounded frame reception and validation, a sensor snapshot from the
   existing I2C owner, and a full-frame operation on the display task. Keep the
   existing voice upload and text response intact.
3. Dispatch cold, timer, AI and Up wakes; preserve the deadline through sleep
   and Up-only wakes. After a voice result finishes drawing, wait ten seconds
   with WiFi retained, fetch the dashboard and sleep after its refresh.
4. Handle AI during the wait and background fetch without dropping capture or
   cutting an active panel refresh. Coordinate network cancellation and reuse;
   do not add a blocking wait on the capture path.
5. Preserve the current screen on failure, add the reserved status indicator
   and keep the silent icon working offline. Bound all background attempts
   and return to the hourly retry schedule after failures.
6. Choose the dashboard layout and data sources, implement background source
   refresh and post-voice cache updates, then validate the complete device flow.

### Acceptance

- Server tests cover authorization, telemetry validation, exact frame format
  and headers, stale/unavailable sources and refreshed data after a voice action.
  Use fake sources, without real services or secrets.
- Firmware checks cover short, oversized and unsupported frames, read stalls,
  invalid/missing/overflowing intervals, and the deadline calculation including
  drawing time and intervening Up wakes.
- On the device: cold startup, AI wake, repeated timer wakes on battery, ten
  seconds measured from display completion, AI during waiting/downloading,
  WiFi/server failures, and preservation of surrounding pixels during local
  icon updates. Background refreshes are silent and never start the microphone.
- Verify full-frame orientation and polarity, clean full refreshes, the silent
  icon and stale indicator visually. A successful build does not complete S15.

Compression and energy comparisons are deferred to E10 and E9 respectively in
[experiments.md](experiments.md); D10/D11 track the initial choices. Autonomous
reminders and server-controlled answer dwell time are outside this step.

### Implementation notes — 2026-09-22

The server now exposes GET /sticky/dashboard, with a fixed mono1-v1 frame or
an equivalent 1-bit PNG preview. It renders a diagnostic layout with telemetry;
real content, sources and cache invalidation remain outside this step.

Dashboard owns the HTTP worker and PSRAM frame. It validates status, format,
MIME type, identity encoding, fixed length and complete receipt before Display
can see the pixels. AI cancels it without joining on the capture path. Socket
shutdown interrupts headers/body reads; a connect still in progress exits on
its own timeout. The 15-second request budget includes headers and download;
WiFi has its own 15-second budget. Redirects and chunked frames are rejected.

The orchestrator now returns from each voice cycle into loop(), avoiding
recursive cycles. It waits from Display's final refresh timestamp, retaining
WiFi, then fetches and shows the dashboard. Cold/timer wakes take that path
without capture or chirps. Up wakes remain offline and preserve the schedule;
Up during the awake idle phase also toggles sound. Sensor reads are posted to
Display, preserving the existing sole I2C owner.

The deadline lives in RTC memory as raw slow-clock ticks; drawing, awake work
and intervening Up sleeps all spend that interval. Raw ticks avoid rescaling
absolute uptime after boot recalibration. Valid intervals clamp to 60–86400 s;
missing/invalid/overflowing values and failed requests use one hour. An overdue
deadline arms a 60-second timer instead of a wake loop.

Dashboard is always a full refresh. Reserved white rectangles belong to the
silent and failure indicators. Failed fetches retain the glass and mark only
an existing known dashboard; a cold boot may show NO DASHBOARD. Both indicator
partials use the driver's full-plane shadow priming. Unknown indicator state
now preserves the glass instead of clearing it.

Validation: 94 server tests and 24 native firmware tests pass. All six embedded
environments build, including the final production and E1 firmware variants.
The Docker image also passes an offline route smoke test for raw/PNG, both
authentication methods and the existing audio fault endpoint. Server tests
compare every PNG/raw pixel and exercise auth,
telemetry and headers; native tests cover frame bounds, metadata, intervals,
remaining time and the existing driver RAM invariants. These checks do not
establish cancellation latency, actual RTC timing, physical polarity or button
behavior. No serial device was connected during implementation, so flashing,
battery timer wakes, voice interruption and visual acceptance are still due.
S15 is not marked hardware-complete.


### Dashboard follow-up — taps and request timing, 2026-09-22

The user tested the initial dashboard on the device and reported that it works
well, but a discarded AI tap still drew the old local notebook. Removed that
screen from Display, StickyScreen, its shared icon and the host preview.
All returns to idle now fetch the server dashboard:

- Cold startup and timer wake already used the fetch path.
- An AI press released before setup now fetches instead of preserving the glass.
- A discarded tap after capture starts now fetches directly, with no audio
  upload and no ten-second answer dwell, including taps during an idle cycle.
- A completed voice answer/error retains the ten-second dwell, then fetches.
- Up-only wake still updates the silent indicator offline and preserves the
  dashboard deadline; it never used the notebook path.

The success log now reads `dashboard: 48000 bytes, HTTP request <ms> ms, next
update in <seconds> s`. Both timestamps come from the network worker: just
before HTTP open and immediately after receiving the response body. DNS/TCP,
server processing and transfer are included; WiFi startup, worker scheduling,
main-loop polling and e-paper rendering are excluded. The timing is published
with the completed response and reset for each new request.

Validation: all six embedded environments build, all 24 native tests pass,
and the host preview builds and produces its two remaining image sheets.
A source search confirms no notebook drawing or local idle-screen API remains;
`git diff --check` passes. The earlier successful hardware report does not
cover these refinements; no USB serial device was connected to flash them.


### E2 — verified HTTPS timing, 2026-09-23

Added an isolated `exp_e2` environment using production Backend, WifiLink and
Recording, with linker-injected certificate trust and a connection timestamp.
The rig runs 12 HTTP/HTTPS pairs for a tiny request and 12 for a paced four-second
stream, one request per wake, against the production fault endpoint. It leaves
normal firmware behavior unchanged. `tools/e2_summary.py` reproduces summaries
from the checked-in serial logs.

Both the weak-signal 2026-09-22 run and the good-signal 2026-09-23 rerun completed
all 48 requests with expected HTTP 500 and full uploads. The first attempted
rig placed Backend on the loop stack and overflowed it before measurements;
the measured rig uses static storage, matching the other rigs. Good-signal
medians: 265 ms HTTP versus 1062 ms verified HTTPS for the tiny request, and
76 versus 80 ms from the nominal last streamed sample to response headers.
About 53 KiB additional internal RAM is needed. Full methodology, ranges,
clock cost, limitations and migration implications are in
[E2](experiments.md#e2----https-overhead). After reviewing the results on 2026-09-23, the user chose
to retain HTTP and defer the HTTPS migration under D5. The experiment and standard environments build successfully;
standard firmware was restored after capture. Its cold-boot UART log confirms
a successful 48000-byte dashboard download (1391 ms HTTP request), display
refresh and return to sleep. No display changes need acceptance.

### Dashboard PNG trial — 2026-09-23

Firmware now explicitly requests `?format=png` from the existing server. This
change is firmware-only: the server still supports and defaults to mono1-v1.
PNG acceptance precedes the separately planned server default/removal change.

The accepted image is exactly the server's Pillow mode-1 output: 800x480,
grayscale bit depth 1, no interlace/transparency, IHDR/IDAT/IEND only. The worker
checks HTTP metadata and complete Content-Length (1..65536 bytes), PNG chunk
ordering/CRC, the zlib checksum and exact inflated length. Split IDAT and all
five row filters work. PNG white bits are inverted into the existing MSB-first,
1=black 48000-byte display buffer; panel drawing and local indicators are unchanged.

Compressed bytes, inflate state and scanlines use bounded PSRAM allocations.
Inflation checks cancellation every 1024 output bytes, unfiltering every row;
download and decode share the existing request deadline. Failure never posts
partially decoded pixels to Display. Temporary decode workspace is freed on
every exit; close releases the download buffer after worker completion and
retains the pixels until Display is finished. The inflater is miniz vendored
unchanged from the already pinned Seeed_GFX2 Sticky example, with provenance
and its license retained. It is compiled identically in native tests.

The success log reports compressed bytes, HTTP request milliseconds and decode
milliseconds separately. The update deadline starts at complete body receipt,
so decoding and panel work spend the interval too.

Validation: all 28 native tests pass; all seven embedded environments build.
Fixtures from the actual server encoder compare all 48000 decoded bytes.
A deterministic random image covers all filters, multiple IDAT chunks and a
49302-byte PNG, ensuring incompressible content fits. Tests also cover damaged
CRC/zlib/Adler32, trailing data, truncation, unsupported headers/chunks, wrong
decoded size, invalid filters, allocation failure and cancellation cleanup.
Flashed standard firmware to the connected ESP32-S3 and captured a cold-boot
UART smoke test: PNG 2323 bytes -> 48000 bytes, HTTP request 495 ms, decode
14 ms, successful 2725 ms full refresh, then deep sleep with the remaining
dashboard deadline. The display task reports 5900 unused stack bytes.
The filtered [device log](measurements/dashboard-png-2026-09-23.log) is retained.
This establishes a successful real-server/device round trip; it does not
establish energy savings or a controlled latency comparison with mono1.
Visual acceptance and physical button interruption remain user checks.

### PNG accepted; remove format selection — 2026-09-23

The user accepted the PNG dashboard on the device and reported a much faster
request. Removed the server's mono1 encoder and the endpoint's `format` query
parameter. GET /sticky/dashboard always returns the same 1bpp PNG; browser
Basic authentication now works on this plain URL, while device Bearer auth is
unchanged. Unknown query parameters are ignored by FastAPI, so the installed
trial firmware's old `?format=png` URL also receives PNG after deployment.

Firmware now requests the plain endpoint, adding only available sensor values
with the appropriate query separator. It already rejected mono1 responses.
Raw expected-pixel test fixtures are named `.bitmap` and generated directly
from rendered pixels; they no longer depend on a server wire encoder.
Updated the shared contract, scenario, vision and Docker CI smoke test; D10 is
resolved. Representative energy measurements remain E10 work.

Validation: 93 server tests and 28 native firmware tests pass; all seven
embedded environments build. The server tests
check the exact PNG subset accepted by the firmware, sensor rendering, auth,
the absence of format selection in OpenAPI and PNG responses for old URLs.
Deploy the server before installing firmware without `format`: the old live
server defaults to raw pixels. The accepted trial firmware remains installed
until that server rollout; no deployment or flashing was performed in this step.

## Dashboard sensor indicators — 2026-09-23

Moved battery and climate rendering to the server dashboard, preserving the top
margin layout, battery fill and unknown-value labels. Firmware samples sensors
only for dashboard requests; local voice screens retain the silent indicator.
Removed the generated FreeSans Bold 12 pt face and its preview/generator entries.
Validation: 99 server tests and 28 native firmware tests pass; the production
firmware and host preview build successfully. The server PNG was visually checked.
No USB device port was present, so flashing and visual device acceptance are pending.


### RTC PNG restoration experiment rejected — 2026-09-23

Tried retaining the server PNG in a single 7168-byte RTC FAST buffer and
restoring the previous dashboard after deep sleep so LISTENING could use
partial refresh. The image looked correct, but four measured transitions took
2445–2477 ms. PNG copy/decode was only 16.515–16.849 ms; bitmap/overlay redraw
cost 745.031–760.202 ms, bringing total restoration to 903.147–920.267 ms.
The user found it perceptually slower than full refresh, whose text becomes
readable before its update finishes.

Rejected at the user's request. Removed the RTC cache, input-preserving decoder
change, shadow restoration, timing instrumentation and experiment-only tests;
restored the previous full LISTENING refresh and existing partial answer/working
paths. Server PNG transport and its original decoder remain unchanged. Results,
limitations and filtered measurements are recorded in
[E11](experiments.md#e11----rtc-png-restoration-before-listening-rejected).

Rollback validation: production firmware built successfully and all 28 original
native tests passed. Only experiment documentation and its filtered log remain
changed; production code and tests match the pre-experiment revision. Uploaded
the restored firmware on 2026-09-23; esptool verified its flash hash and reset
the board.
