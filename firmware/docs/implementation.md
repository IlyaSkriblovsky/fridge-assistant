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
| S5 | Screens | Listening, answer, error | Not started |
| S6 | WiFi | Association without blocking, BSSID cached | Not started |
| S7 | Upload and answer | The backend round trip | Not started |
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
  apart.
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
