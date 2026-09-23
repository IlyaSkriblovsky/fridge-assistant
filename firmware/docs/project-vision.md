# Project vision

A push-to-talk voice assistant living on a Seeed reTerminal Sticky (E1005): an
ESP32-S3 with a 3.97" 800x480 monochrome e-paper panel, a PDM microphone, a
buzzer, three buttons and a battery.

The device sleeps. You hold the AI button, speak, and let go. It sends the audio
to a backend we control, which runs it through an AI model and answers with
text. The answer is drawn on the e-paper, then replaced by a server-rendered idle
dashboard after ten seconds. The dashboard remains readable without power.

It is a personal device, built for one user, powered by battery.

The idle dashboard transport is implemented and the user reports a successful
device check. The latest tap-routing and timing refinements await a device recheck.
Content and layout beyond the diagnostic frame are a separate step.

## Interaction flow

Cold startup and timer wakes fetch a dashboard without recording or chirping.
An AI wake already released at setup and a discarded short press also fetch
the dashboard, without an answer dwell or audio upload. There is no local
notebook screen. Voice screens retain only the local silent-mode indicator.

1. **Asleep.** Deep sleep, woken by AI (GPIO4), Up (GPIO5), ext1 any-low, or the dashboard timer.
   Up takes the silent-mode path described below.
2. **Press.** Wake and hold the power latch.
3. **Record.** Power the microphone, wait out its settle window, then start
   capturing 16 kHz mono PCM into a linear buffer in PSRAM -- and only then
   chirp. The chirp means "the microphone is live", so it cannot come earlier
   without inviting the user to talk into a microphone that is not listening
   yet. Everything before it is dead time, which is what
   [E1](experiments.md) and [E3](experiments.md) exist to shrink.
4. **Connect.** Bring up WiFi concurrently with recording, in a separate task.
   Draw the "Listening" screen whenever the panel gets round to it; it will be
   late and that is accepted. Once the network is up and the press has lasted
   long enough to count, open the request and stream the recording into it as
   it is captured.
5. **Release.** Debounce, then stop capturing.
6. **Working.** Say that the question was taken and is being answered, twice
   over: a chirp the moment the button comes up, and the word on the panel
   changing from "Listening" to "Working". The backend runs speech recognition
   and a language model, which is seconds -- `kResponseTimeoutMs` allows thirty
   of them -- and the panel would otherwise still read "Listening", which stops
   being true the moment the button comes up. The word is a partial refresh, not
   a screen of its own: the answer queues behind it on the one controller, and a
   full refresh would put two and a half seconds in front of the answer's own.
   It is drawn on the panel's own task, so the upload never waits for it, and it
   is skipped when the answer arrives before the panel is free to draw it. The
   answer chirp then marks the end of the wait rather than covering it: it
   sounds when the refresh starts, so it says the answer has arrived and not that
   it is finished being drawn.
7. **Upload.** Send what is left of the recording -- the last few chunks,
   which is milliseconds -- and end the body.
8. **Answer.** The backend replies with text. Render it on the e-paper.
9. **Dashboard and sleep.** Wait ten seconds from the final display refresh,
   keeping WiFi and responding to AI, fetch the dashboard, wait for its full
   refresh and sleep with the latch held. A failed fetch preserves the screen.

Press and hold is the simplest interaction that has a beginning and an end. It
is not settled that it is the right one -- [D9](deferred.md).

Measurements the design still waits on are tracked in
[experiments.md](experiments.md).

## Idle dashboard

**Implemented 2026-09-22; user reports successful operation on the device.** The shared scenario is in
[idle-screen.md](../../server/docs/use-cases/idle-screen.md), and the
wire format is in [device-contract.md](../../server/docs/device-contract.md#дашборд).

The backend owns data sources, fonts and layout and returns a complete 800x480
monochrome image. The device supplies battery, temperature and humidity,
validates the frame, displays it and sleeps. Dashboard changes need only a
backend deployment. Voice answers keep their current local text rendering.

- Cold startup and timer wake fetch a dashboard without recording or chirping.
- AI starts capture immediately, with no dashboard fetch ahead of it. It also
  takes priority during a background fetch or the post-answer wait. The panel's
  current waveform is allowed to finish on the display task.
- After the final voice screen finishes drawing, stay awake for **10 seconds**
  with WiFi retained, then fetch and display the dashboard. Use the display
  task's completion timestamp, not receipt of the voice response. The wait must
  remain responsive to AI; a discarded tap is not a completed voice cycle.
- Up keeps its existing offline silent-mode behavior, including the current
  simultaneous AI/Up wake rule. It must preserve the dashboard's pending
  deadline across its own return to sleep.
- Start with hourly updates. `Next-Update-After` sets the next request interval
  in seconds from complete receipt of a valid frame. Subtract work already
  done before arming deep sleep, and retain button wake alongside the timer.
- Log the HTTP request duration on the network worker, from opening the
  request to receiving the complete response. This includes DNS/TCP, server
  processing and transfer, and excludes WiFi startup and panel rendering.
- Receive and validate the entire frame before displaying it. Draw the
  dashboard with a full refresh and wait for completion before sleeping.
- Bound connection and download time. Failure preserves the previous screen
  and returns to sleep for an hourly retry, without a background error chirp.
  A small local indicator can mark a failed update on an existing dashboard;
  reserve its region together with the silent indicator in the future layout.

The diagnostic backend frame incorporates the supplied sensor snapshot. External
data and background refresh belong to the later content step. Keep sensor I2C
access under one owner rather than reading from networking and display tasks
concurrently. Partial indicator updates after sleep must retain the driver's
full-plane shadow-prime initialization and known previous indicator pixels.

PNG transport was accepted on the device on 2026-09-23. The awake-wait choice
is tracked in [D11](deferred.md). Measurements come after the dashboard design;
no energy advantage of remaining awake has been established. Offline reminder
storage and alarms are a separate future feature, not part of this plan.

## Decisions

### Transport: HTTP POST, not WebSocket

A WebSocket buys streaming at the cost of a library, frame handling, a bespoke
envelope protocol, pings and reconnect logic -- all for a single request and a
single response per session. It is not worth it here. `esp_websocket_client` is
not even shipped with the Arduino framework, so it would be an added dependency.

One POST with `Transfer-Encoding: chunked`. The request opens while the button
is held, the body streams as audio is captured, and the terminating chunk *is*
the "transmission over" signal, so there is no separate protocol and the
response body is just the answer. What the user waits for after the release is
the last few chunks and the backend: the answer chirp sounds 115 to 154 ms after
the release whatever the hold, 60 of them the taken chirp, against the
prototype backend that answers a fixed phrase
([S11](implementation.md#s11----streaming-upload)). Until S11 the whole
recording went up after the release, and the upload was the largest term the
device controlled -- 2.9 s for 400 KB, with a retransmission timeout in one
upload in four ([E7](experiments.md)).

`esp_http_client` is what sends it, from the IDF underneath Arduino: it opens a
chunked body with `esp_http_client_open()` and a negative length, and leaves the
chunk framing to the caller. Arduino's own `HTTPClient` sends only bodies of
known length. Servers and proxies also tend to buffer a whole chunked request
body before handing it to the application, which turns streaming silently back
into a plain POST; since the backend is ours, that is a configuration matter (in
nginx, `proxy_request_buffering off`).

### The request contract

```
POST {secrets::kBackendBaseUrl}{config::kAudioPath}     ->  POST /audio
Authorization: Bearer {secrets::kDeviceToken}
Content-Type: audio/wav
Transfer-Encoding: chunked

<WAV: PCM, 16 kHz, mono, signed 16-bit little-endian>
```

Sample rate and format travel in the WAV header rather than in custom headers,
so the body is self-describing and can be saved and played back as a file on the
backend side.

**The header says `0xFFFFFFFF` in both of its length fields**, because it goes
up before the recording has a length. The transport carries the length instead:
the terminating chunk is where the body ends, so the backend takes the length
from there and rewrites or strips the header as it needs -- the prototype
rewrites both fields, so the file it keeps is an ordinary WAV. `0xFFFFFFFF`
rather than zero because Python's `wave`, which the prototype reads its uploads
with, takes the first as a file of unknown length and reads every sample in it,
and refuses the second outright as `not a WAVE file` -- checked with a
one-second tone, both ways.

Raw PCM with the format in request headers was the other way, and it buys
nothing. The header would only move to the backend, which needs the format
either way -- to save a file that plays now, and to hand the audio on later --
and there is no standard type for little-endian PCM to carry it in:
`audio/L16` is big-endian by RFC 2586. So it would mean swapping a megabyte on
the device or headers of our own, and the second is the bespoke protocol the
transport decision above exists to avoid.

A stream that dies mid-question leaves the backend a truncated body. The
prototype keeps it, with its lengths filled in from what arrived; the real
backend will forward the stream to an external API rather than keep
recordings, so nothing has to be done about it.

The request carries one credential, a static token in `src/secrets.h` that the
backend holds as well, and the backend turns away anything without it with a
401. The backend lives on a public host, so the WiFi password no longer draws the
trust boundary. One device and one user need nothing finer than a shared
secret: no accounts, no expiry, no exchange.

The token goes over plain HTTP until [D5](deferred.md) is paid off, so anyone
who can watch the traffic can read it. It keeps out whoever merely finds the
address, which is the likely visitor, and HTTPS waits for [E2](experiments.md)
to say what the handshake costs a device that wakes for every question. Anyone
holding the device can read it from the flash as well, like everything else in
`src/secrets.h`.

The response is JSON with the answer in `response`:

```json
{ "response": "..." }
```

Anything else -- a non-200 status, a body that does not parse, or valid JSON
without a `response` field -- is one error to the device, shown on screen. Richer
error reporting can come later.

### Transport security

Plain HTTP to start -- [D5](deferred.md). HTTPS is wanted, because the backend
lives on a public host rather than at home and the device's token crosses the
internet in clear, but on a device that wakes from deep sleep for every question
the TLS handshake is paid every single time. Whether that cost is
acceptable is [E2](experiments.md); the alternative is a proxy on the home
network, which is one more component to maintain and so a worse answer if the
number turns out to be small.

### Audio buffer: linear, capped at 30 seconds

PSRAM makes a ring buffer unnecessary -- there is no memory ceiling to slide
against. A linear buffer with a hard cap is less code and avoids reasoning about
a write pointer overtaking a drain pointer.

16 kHz, 16-bit, mono is 32 KB/s, so the 30 s cap is 960 KB. Recording stops at
the cap whether or not the button is still held.

The buffer holds raw signed 16-bit little-endian PCM and is sent as a WAV. The
44-byte header is at the front of the allocation, written when the buffer is,
so that sending never copies a megabyte to prepend it; it declares no length
(the request contract above has why), so it never has to be written again.

The buffer is written by the capture task and read by the upload at the same
time. The capture task publishes how much of it has landed, with the ordering
that makes everything below the count safe to read, and samples never move
once they have landed -- so the count is all the two share, and the capture
task never waits for the reader.

### Power: deep sleep

Battery-powered, so the device sleeps between questions.

Deep sleep on this board is not just `esp_deep_sleep_start()`: the power latch
pins have to be latched *through* the sleep with `gpio_hold_en()` and
`gpio_deep_sleep_hold_en()`, or the board switches itself off instead of
sleeping. Seeed's own firmware also parks EPD_EN, TOUCH_EN, TOUCH_RST, SD_EN and
BUZZER low for the duration.

Waking is not instant -- image load from flash, latch, microphone rail, then the
settle window the driver discards. The first fraction of a second of speech is
lost, which is why the buzzer chirp comes first: it tells the user when to
start. On the finished firmware the chirp sounds 104 ms after the wake event,
the same to a fifth of a millisecond every time: 61 ms of boot, 41 ms of
microphone rail and settle discard, and under 2 ms of everything else --
[E1](experiments.md), re-run at [S9](implementation.md#s9----re-run-e1). Two
delays this project chose, 100 ms in the latch and 50 ms after the log port came
up, were taken off the wake path on the way there.

Caching the BSSID and channel in RTC memory across sleeps and passing them to
`WiFi.begin(ssid, pass, channel, bssid)` removes the scan, and reconnect time is
what the buffer exists to cover.

It is worth less than this document assumed, and for an instructive reason. On
the home network the cache is the difference between an 87 ms link and a scan of
100 ms to 1.3 s, while the address behind it takes 3.15 s of DHCP every time --
so the connect is 3.2 s either way and the cache saves a tenth of it
([S6](implementation.md#s6----wifi)). The association was never the expensive
half.

**So the address is cached beside it**, which is the other nine tenths.
[E6](experiments.md) took the 3.2 s apart -- 2.1 s of the router thinking about
its first answer, a full second of lwIP's ARP check -- and none of it is ours to
make faster; what is ours is not asking. The lease the last wake was given is
kept in RTC memory and installed with `WiFi.config()` before the association, and
the device is on a usable network 300 ms after the wake instead of 3.5 s
([S7b](implementation.md#s7b----cached-dhcp-lease)).

It is not a claim on a fixed address. The device only ever reuses what this
network gave it, for less than half the life the server put on it, and a lease
that has outlived its network is dropped by the first question that cannot reach
anything through it -- which costs that one question and nothing afterwards. The
upload is the probe, so nothing on the happy path pays for the check.

**The DNS server can be set in `src/secrets.h`**, in place of the one the
network hands out. The home router stops answering DNS now and then while it
goes on routing everything else, and the backend is reached by name, so a
question asked during one of those spells ends in NO SERVER on a network that is
otherwise fine. Empty keeps the network's. It goes in once the address is in
hand, because DHCP writes its own over anything installed earlier
([S12](implementation.md#s12----dns-server)).

### Framework: stay on Arduino

`framework = arduino` in arduino-esp32 3.3.7 *is* ESP-IDF 5.x with a library
layer and a `loopTask` on top. FreeRTOS is fully available either way, so
switching to `framework = espidf` would buy nothing for concurrency.

What keeps us on Arduino is Seeed_GFX2: the entire display stack, including the
panel and driver classes this project subclasses, is an Arduino library. Moving
to plain ESP-IDF would mean rewriting the panel driver, which is exactly what
Seeed did for their own IDF firmware.

Where an Arduino wrapper is inadequate, the IDF API underneath is included
directly -- `esp_http_client` for chunked uploads, `esp_sleep` for deep sleep,
FreeRTOS for tasks. PlatformIO also accepts `framework = arduino, espidf` if
menuconfig-level tuning is ever needed.

### Concurrency: voice tasks and the dashboard worker

Capture and networking cannot share a thread. The I2S DMA holds 6 x 240 frames,
which is 90 ms at 16 kHz, while a blocking WiFi connect takes seconds. Audio
would be dropped in chunks.

- **Capture task**, pinned to core 1 above everything else there, reads I2S
  into the buffer and polls the button between reads. A `gpio_get_level()`
  costs nothing and never blocks, and it buys a stop that does not depend on
  what the other tasks are inside: a release arriving during a refresh or a
  stalled upload would otherwise add a second or two of room noise to the end
  of every recording.
- **The orchestrator**, Arduino's own loop task on core 1, does WiFi and HTTP
  and decides what happens next. The WiFi and lwIP tasks sit on core 0, so the
  recording and the network never share a core either.
- **Display task**, core 0 below everything but the idle task, owns the e-paper
  and draws whatever the orchestrator posts: at most one screen waiting, and a
  newer post replacing one the panel has not reached yet.

E-paper refresh is a long blocking SPI transfer followed by a BUSY wait, 0.8 to
2.3 s a screen. It must not sit between two I2S reads, and it does not sit in
front of the upload either: the orchestrator posts a screen and carries on, and
the one place it waits for the panel is before deep sleep, which would otherwise
cut a refresh in half. Almost all of a refresh is the library polling BUSY with
`vTaskDelay()`, so the task is blocked for nearly its whole life and wants no
priority to speak of.

A separate dashboard worker performs a bounded PNG GET and decodes it into a
48000-byte PSRAM pixel buffer. The dashboard endpoint always returns PNG: 800x480 grayscale, one bit per pixel, without transparency or
interlace. Download and decode share the request deadline; temporary compressed
data and inflate workspace also live in PSRAM. The worker receives a sensor
snapshot from Display and never touches I2C.
The orchestrator polls buttons during downloading, dwell and panel refreshes.
Cancellation shuts down an established socket without waiting for worker exit;
its client and buffer stay alive until it reports completion. Display borrows
the immutable pixels until idle; a new download cannot reuse them before then.

The third task came before the streaming upload rather than with it.
[S8](implementation.md#s8----the-flow) measured the panel as most of what the
user waited through after the release -- the rest of the Listening refresh on a
short question, the working screen on every one -- and none of it needed the
orchestrator's thread; [S10](implementation.md#s10----display-task) took it
off, and the wait became the round trip. The streaming upload that
[S11](implementation.md#s11----streaming-upload) then took off the round trip
runs on the orchestrator, which had nothing else to do while the button is
held. Its writes block that thread, through a stall if there is one, and that
is allowed: the release is timed by the capture task, and the buffer is linear,
so there is nothing for a late write to overrun.

### Buttons

Active low with internal pull-ups. The AI button is GPIO4, the side buttons are
GPIO5 and GPIO6.

The press that wakes the device is still held when `setup()` runs, so the
firmware reads the level rather than waiting for an edge. GPIO4 also comes back
as an RTC pad, which is what ext1 and its pull-up need across the sleep:
`rtc_gpio_deinit()` hands it to the GPIO matrix before `digitalRead()` means
anything.

Debouncing matters on release as well as on press: without requiring a stable
high for 30-50 ms, a bounce ends the utterance mid-sentence. A minimum hold of
roughly 300 ms should also be required so an accidental tap does not wake the
whole pipeline.

### Screen

The current voice flow has four screen transitions: asleep -> Listening,
Listening -> working, working -> answer, working -> error.

**One of them is a full refresh and it is the first one.** A partial update is
differential -- the controller picks each pixel's waveform from the pair (what
is on the glass, what should be) -- and after a deep sleep the firmware knows
nothing about what is on the glass: the previous-image plane is rebuilt from
zero on every boot while the panel still holds the last answer. Only a full
refresh drives every pixel whatever it was, so exactly one is needed per wake to
reconcile the two, and it is `Listening`. It also clears accumulated ghosting,
which is the other thing full refreshes are for, and it fixes the ghosting depth
at two by construction: full, partial, partial, sleep, whatever the sequence of
questions, so nothing accumulates across a run.

That is the cheap place to spend it. The Listening refresh runs under the
recording, where the user is still talking; everything after it is on the far
side of the button coming up, where a second is a second before the answer is on
the glass.

**So working, answer and error are partial**, and on this panel that is 1344 ms
against 2400 ms for the same area full -- measured at
[S8](implementation.md#s8----the-flow), where taking the two window sizes apart
also showed the difference is waveform and not transfer. Every transition comes
out clean: no smear, no residue, checked by eye over a run of consecutive
questions. That run was the first time the partial-refresh correction in
`src/sticky/epaper.h` had ever executed.

**The working transition is what settled the shape of all of this.** Every
other transition happens while the user is waiting for nothing; this one is
between the button and the answer. Until [S10](implementation.md#s10----display-task)
it sat on the critical path -- released, draw, upload, wait, draw again -- and
whatever it cost was added to the wait for every answer. With the panel on a
task of its own it no longer delays the answer's chirp, but it still delays the
answer's appearance, because the screens of one question queue on one
controller. It repaints the word and nothing else: `LISTENING` and `WORKING` are
one screen with two words in it, drawn in the same face at the same size and
centred, so the strip of panel the word occupies belongs to the face rather than
to the word, and a partial refresh of that strip -- 990 ms -- is the whole
transition. When the answer arrives before the panel is free to draw the word,
the word is dropped: on a short question it is still under the Listening refresh
when the answer lands, every time.

The two shapes it was chosen over were a screen of its own at the full 2.4 s,
which would have been slower than the wait it announced three times in four, and
no screen at all with only a chirp, which would have left the panel saying
`LISTENING` after it had stopped being true. The chirp is there as well; it is
not an alternative to the word but the part of the answer that arrives
immediately.

**Landscape, with the three buttons along the top edge on the left.** After
using the device, this proved more comfortable than the original buttons-down
orientation. `StickyScreen::begin()` calls `setRotation(2)` to turn the image
180 degrees from Seeed_GFX2's board default, preserving the board profile's
horizontal mirror correction and the 800x480 layout.

The answer stays on screen for ten seconds after its refresh, then the
[Idle dashboard](#idle-dashboard) replaces it with a full refresh. Listening
replaces the current screen when a new question starts. Cold startup, timer
wakes and discarded taps all fetch a fresh dashboard; no path draws a local
notebook. Up-only wakes retain their offline indicator update.

The dashboard is not reconstructed after sleep: its 48000-byte frame cannot
fit in the RTC memory already shared with other retained state. Listening
therefore remains a full refresh when the next voice question starts. Local
indicator-only updates can reconstruct their reserved regions using the
existing shadow-prime path. The added wake time and refreshes will be measured
in [E9](experiments.md#e9----dashboard-energy-deferred); they are not a blocker
for the first version.

Battery, temperature and humidity appear only on the server-rendered dashboard:
a filled battery icon and percentage at the upper left, `23.4°  48.2%` at the
upper right. The server owns their FreeSans Bold font. Voice screens do not
render these indicators. The display task reads the sensors once before each
dashboard request, retaining sole ownership of sensor I2C. An Up-only update
preserves the surrounding dashboard pixels.

The BQ27220 uses sensor I2C (SDA 1, SCL 0), address `0x55`, register `0x2C`,
two bytes little-endian. The SHT40 uses address `0x44`, high-precision
measurement with the heater off. Failed readings are omitted from the request;
the server shows `?` for battery and `--°  --%` for missing climate readings.
Gauge accuracy and low-battery behaviour remain [D3](deferred.md).

The wider UI is deliberately unconsidered until the proof of concept works.

### Text rendering

Answers can be in Russian, so the display renders Cyrillic. It renders Greek
and the typography a Russian sentence brings -- guillemets, the em dash, the
numero sign -- for the same reason and at the same time, because the work is
in the machinery and not in the glyphs. Anything else is drawn as `?`, one per
character.

The faces are GNU FreeFont, the design Seeed_GFX2 already bundles, regenerated
with those scripts in them by `tools/gfxfont.py`. Layout is the firmware's own,
in `src/text.h`, which is where both reasons for that are written down.

An answer is a sentence or two rather than a phrase, so it is drawn as lines:
broken at spaces to fit a box with a margin on all four sides, left-aligned
because a ragged right edge reads as text where a centred one reads as a
poster, and centred in the box as a block so that a short answer still sits in
the middle of the panel. Seven lines of 24 pt is what the box holds. An answer
longer than that ends in an ellipsis, which says the panel has more to show and
no way yet to show it -- [D2](deferred.md).

`SmoothFont` was the obvious path and is the wrong one. It loads VLW fonts,
which carry eight bits of alpha per pixel for anti-aliasing, and the panel is
one bit: `Panel_EPaper::writePixel` makes every colour but pure black white, so
every blended edge pixel disappears and only the fully-opaque interior of a
glyph survives. It would cost eight times the storage, a parse on every boot
and about 2 KB of RAM to arrive at a one-bit render either way.

### Sound

The buzzer on GPIO48 is the only feedback fast enough to be useful -- the e-paper
is one to two seconds behind everything. Three patterns, distinguishable without
looking:

| Event | Pattern |
| --- | --- |
| Ready to listen | two very short notes, low then high |
| Question taken | one short note, between the two |
| Answer received | two short notes, high then low |
| Error | one longer note |

The rising pair opens a question and the falling pair closes it, so the two
normal outcomes are opposites, and the single sustained note is neither.

The taken note has to survive being heard about half a second before the
answer's pair, which is what the round trip usually is ([E7](experiments.md)),
so it is placed where neither pair can absorb it: one note rather than two,
between the pair's two pitches so it is neither of them, and far too short to be
the error's sustained note.

The ready chirp sounds *after* capture has started, so the microphone records
it. That is the cheaper trade: chirping first and then starting capture would
add the chirp's own duration to the dead time at the front of every question.
The notes are short, and the backend can drop the head of the recording if it
ever matters.

**Every other chirp sounds before its screen, the answer's included.** The
buzzer exists because the panel is one to two seconds behind; holding a chirp
back until the refresh returns spends that advantage and makes the wait feel
longer than it is. It costs nothing in accuracy, because a full refresh is
readable well before it ends -- the text appears inverted partway through -- so
by the time the user has looked up, the answer is already on the glass. Judged
at the panel during [S7](implementation.md#s7----upload-and-answer), where the
first real wait existed to sit through.

Since the panel has a task of its own the screen is posted a moment before the
chirp rather than drawn after it, so the panel starts on the image while the
chirp sounds; the image still arrives a second behind the sound, which is the
order that matters.

A press too short to count makes no sound at all -- [D7](deferred.md).

Drive the buzzer through LEDC rather than `tone()`. `tone()` hands the note to a
background task and returns before the sound ends, so a chirp issued just before
`esp_deep_sleep_start()` is either cut off or still sounding when the pads are
parked.

### Errors

Every failure does the same three things: chirp, draw the message, sleep.

| Situation | Screen |
| --- | --- |
| WiFi did not associate | `NO WIFI` |
| WiFi dropped while recording | `NO WIFI` |
| Backend unreachable | `NO SERVER` |
| Backend answered with a non-200 status | `SERVER ERROR` plus the status code |
| Response did not parse, or has no `response` field | `BAD RESPONSE` |
| No answer within `kResponseTimeoutMs` | `TIMED OUT` |
| Microphone did not start | `NO MICROPHONE` |
| The audio buffer could not be allocated | `NO MEMORY` |

A network failure during recording aborts immediately rather than letting the
user finish talking into a recording that has nowhere to go. It costs an
interrupted sentence, but the alternative is a long silence followed by the same
error. Since the upload streams under the hold, the backend can fail there too
-- a connection refused, a stream that dies -- and it is the same failure on
the same terms: the error screen comes while the button is still down.

If the display itself fails to initialise there is nothing to draw on; that case
chirps, logs to Serial1 and sleeps.

Two cases are not errors:

- **Press shorter than `kButtonMinHoldMs`.** An accidental tap. No audio is sent;
  the device fetches and shows the dashboard before sleeping. A tap released
  before setup also fetches the dashboard. Neither path has the ten-second
  answer dwell.
- **Recording reached the 30 s cap.** Capture stops and whatever was recorded is
  sent as a normal question.

### Configuration and secrets

Split in two, by whether a value can be committed:

- **`src/config.h`** -- tracked. Backend path, recording cap, response
  timeout, button debounce and minimum hold. Meant to be edited.
- **`src/secrets.h`** -- gitignored. WiFi credentials and an optional DNS
  server, the backend's base URL and token, and whatever else turns out not to
  be committable.
  `src/secrets.example.h` is the committed template and must be kept in step
  when a constant is added. A missing `secrets.h` breaks the build at the
  include; empty values are reported over Serial1 at runtime.

Secrets are plain strings in the firmware image and anyone who can read the
flash can read them. Acceptable for a personal device; they should not be
credentials that matter elsewhere.

The backend's base URL sits in `secrets.h` rather than `config.h`: the backend
lives on a public host and the repository is public, so the address stays out of
it. The token is what actually keeps strangers out; not publishing the address
only means fewer of them try. The path is not secret and stays in `config.h`.

## Hardware notes

Pin map below is from Seeed's own firmware
([reTerminal_Sticky_Bunny](https://github.com/limengdu/reTerminal_Sticky_Bunny),
`src/board/pin_config.h`) and the
[hardware overview](https://www.seeedstudio.com/sticky/docs/en/device-guide/hardware-overview/),
cross-checked against this project's own measurements.

| Function | GPIO |
| --- | --- |
| Power hold / power lock | 45 / 46 |
| AI button, side buttons | 4, 5, 6 |
| PDM microphone CLK / DATA / power | 19 / 20 / 38 |
| Buzzer | 48 |
| E-paper SCK / MOSI / MISO / CS / DC / RST / BUSY / EN | 13 / 14 / 12 / 15 / 16 / 17 / 18 / 47 |
| Touch GT911 SCL / SDA / INT / RST / EN | 2 / 3 / 21 / 41 / 42 |
| Sensor I2C (SHT40, LSM6DS3TR-C, PCF8563 RTC, BQ27220 gauge) | SCL 0, SDA 1 |
| IMU interrupt | 7 |
| Charger enable / external power detect | 39 / 9 |
| microSD CS | 8 (shares SCK/MOSI/MISO with the panel) |
| UART0 via CH343P bridge TX / RX | 43 / 44 |

### Traps

Three of these cost real time to find and are fixed in this repo. See
`src/sticky/epaper.h` and `src/sticky/mic.cpp` for the details; the short
version:

- **The board switches itself off without the power latch.** GPIO45 and GPIO46
  must go high early in boot. On USB the rail is fed externally so nothing
  appears wrong, which is what makes this easy to miss until it runs on battery.
- **The microphone sits on the USB-Serial-JTAG pads.** GPIO19 and GPIO20 are the
  native USB D-/D+ pins, claimed on boot by this board's Arduino profile
  (`ARDUINO_USB_CDC_ON_BOOT=1`). Releasing the GPIO matrix is not enough; the PHY
  pad enable has to be cleared or the microphone reads silence. Harmless here
  because flashing and logging use the CH343P bridge on UART0.
- **Partial e-paper refresh needs the previous image plane.** An SSD1677 partial
  update is differential over (RAM 0x26, RAM 0x24), but
  `Panel_EPaper::updatePartial()` writes only 0x24. From the second partial
  onwards the controller thinks the panel still shows the last full frame, so
  pixels that went black are never driven back to white and successive frames
  smear on top of each other. `Driver_SSD1677_Sticky` supplies the missing plane.

Also worth knowing:

- **Opening the serial monitor resets the board.** Connecting the cable to a
  running board does not; opening the port does. `monitor_rts = 0` and
  `monitor_dtr = 0` are applied to a port that is not open yet, and opening a
  tty on macOS asserts DTR before they take effect, which the board's auto-reset
  circuit reads as a reset. It arrives through EN, so the chip treats it as a
  power-on and RTC memory is cleared with it. A monitor attached after a battery
  run therefore finds nothing: observing one means the buzzer, flash storage, or
  a monitor opened before the run starts.
- **The sensor I2C bus sits on a strapping pin.** SCL is GPIO0, which the
  ESP32-S3 samples at reset to choose boot mode. The bus has to stay passive
  until boot is over, so the battery gauge cannot be read at the very top of
  `setup()` the way the power latch is.
- This unit has SSD1677 glass. Production mixes SSD1677 and SSD2677 across
  otherwise identical boards, and the two need different drivers. The library's
  auto-detect exists but its SSD1677 path renders inverted, so this project binds
  the corrected driver directly.
- Only `LOAD_GLCD` (font 1) and `LOAD_GFXFF` are compiled into Seeed_GFX2. Fonts
  2/4/6/7/8 are unavailable, so everything is drawn as a FreeFont.
- Two things in Seeed_GFX2's text path are broken for anything but ASCII, and
  both are in non-virtual code. `textWidth()` walks bytes rather than
  characters, so a UTF-8 string measures as empty and `drawString()` centres it
  wrongly; and `drawCharGfx()` reads `GFXglyph::bitmapOffset` with
  `pgm_read_word` although the field is a `uint32_t`, so a font whose bitmaps
  pass 64 KiB silently draws from the wrong offset. `src/text.cpp` works around
  the first and `tools/gfxfont.py` asserts against the second.
- Sources disagree on whether the Sticky has microSD enable and detect pins.
  Seeed's firmware declares GPIO10 and GPIO11; Seeed_GFX2 states the slot has
  neither. Unresolved, and irrelevant until the SD card is used.

### Measured on this unit

- Quiet room reads about -70 dBFS; speech at arm's length peaks near -45 dBFS.
- The ESP32-S3 PDM-to-PCM path has no high-pass stage, so raw samples carry the
  microphone's DC bias. It has to be removed per block or silence does not read
  as silence.

## What is still moving

The voice flow above is implemented. The next agreed change is the
[idle dashboard](#idle-dashboard); its contents and visual design are still
to be chosen. Its architecture is settled, while implementation and device
acceptance remain ahead. Compression and energy measurements are explicitly
deferred, as are offline timers and reminders.

What remains is tracked elsewhere, deliberately kept out of this document so it
does not age every time a shortcut is taken, a number comes in or a step is
finished:

- **[deferred.md](deferred.md)** -- simplifications taken on purpose, each with
  the end state it stands in for and what triggers the change.
- **[experiments.md](experiments.md)** -- measurements the design is waiting on:
  wake latency, HTTPS overhead, microphone settle time, and whether the AI button
  reacts to a long hold in hardware.
- **[implementation.md](implementation.md)** -- the order the firmware is being
  built in, and what each step turned out to involve.

## Silent mode

Up (GPIO5) toggles silent mode when it wakes the device. It suppresses every
buzzer pattern, including errors, and is retained in NVS across power loss.
Toggling never sounds, starts no recording or network request, updates a
crossed-out speaker near x=200 in the top margin, and returns to sleep after
stable button release. The indicator is absent when sound is enabled. Up is
ignored during a question; if both buttons wake the device, Up takes priority.

The indicator owns a separate white rectangle, so its old pixels can be
reconstructed for a partial refresh after deep sleep without knowing the answer
still on the glass. The RAM address window only limits writes: after panel
power loss, both full controller planes must first be initialized identically,
then the indicator window receives its old/new transition. This prevents random
transitions outside the window without storing the answer. Cold or interrupted
updates need full reconciliation.
The usual full Listening refresh still clears accumulated ghosting. Silent
mode intentionally removes the prompt to begin speaking; the user allows a
short pause after pressing AI. Implementation and hardware acceptance are in
[implementation.md](implementation.md#silent-mode--2026-09-21).
