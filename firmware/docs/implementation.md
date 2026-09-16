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
| S1 | Buzzer module | The three patterns, on LEDC | Not started |
| S2 | Button module | A press with a length, debounced | Not started |
| S3 | Capture into PSRAM | A recording, as a WAV in memory | Not started |
| S4 | Capture task | Recording that survives WiFi and the panel | Not started |
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
otherwise add a second or two of room noise to the end of every recording. If
this holds up on the device, it belongs in the vision.

**Verified by** recording while WiFi associates and the Listening screen
refreshes, then checking the buffer for gaps -- this is the step the two-task
split exists for, so a clean recording under both loads is the result.

## S5 -- Screens

`src/sticky_screen.h/.cpp`: `listening()`, `answer(text)`, `error(title, detail)`.
A small interface on purpose -- D4 moves the display into its own task later,
and that is cheap only if nothing else calls the panel directly.

- Full refresh on all three transitions ([D6](deferred.md)).
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
