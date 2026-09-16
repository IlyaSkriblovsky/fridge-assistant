# Project vision

A push-to-talk voice assistant living on a Seeed reTerminal Sticky (E1005): an
ESP32-S3 with a 3.97" 800x480 monochrome e-paper panel, a PDM microphone, a
buzzer, three buttons and a battery.

The device sleeps. You hold the AI button, speak, and let go. It sends the audio
to a backend we control, which runs it through an AI model and answers with
text. The answer is drawn on the e-paper and stays there, readable without
power, until the next question.

It is a personal device, built for one user, powered by battery.

## Interaction flow

1. **Asleep.** Deep sleep, woken by the AI button (GPIO4, ext1, any-low).
2. **Press.** Wake and hold the power latch.
3. **Record.** Power the microphone, wait out its settle window, then start
   capturing 16 kHz mono PCM into a linear buffer in PSRAM -- and only then
   chirp. The chirp means "the microphone is live", so it cannot come earlier
   without inviting the user to talk into a microphone that is not listening
   yet. Everything before it is dead time, which is what
   [E1](experiments.md) and [E3](experiments.md) exist to shrink.
4. **Connect.** Bring up WiFi concurrently with recording, in a separate task.
   Draw the "Listening" screen whenever the panel gets round to it; it will be
   late and that is accepted.
5. **Release.** Debounce, then stop capturing.
6. **Upload.** POST the recording to the backend as a WAV.
7. **Answer.** The backend replies with text. Render it on the e-paper.
8. **Sleep.** Back to deep sleep with the latch held. The answer stays on the
   screen until the next question.

Press and hold is the simplest interaction that has a beginning and an end. It
is not settled that it is the right one -- [D9](deferred.md).

Measurements the design still waits on are tracked in
[experiments.md](experiments.md).

## Decisions

### Transport: HTTP POST, not WebSocket

A WebSocket buys streaming at the cost of a library, frame handling, a bespoke
envelope protocol, pings and reconnect logic -- all for a single request and a
single response per session. It is not worth it here. `esp_websocket_client` is
not even shipped with the Arduino framework, so it would be an added dependency.

**Now:** POST the whole recording once the button is released. Simplest thing
that proves the chain end to end. Latency cost is the utterance duration plus
the upload, paid before the backend sees anything.

**Later:** the same POST with `Transfer-Encoding: chunked` -- [D4](deferred.md).
The request opens on button press, the body streams as audio is captured, and
the terminating chunk *is* the "transmission over" signal, so there is still no
separate protocol and the response body is still just the answer.

Worth knowing now, because it shapes where the code goes: `esp_http_client`
supports chunked request bodies (`esp_http_client_open()` with a negative
length) and is available from the Arduino framework, while Arduino's own
`HTTPClient` only sends bodies of known length. Servers and proxies also tend to
buffer a whole chunked request body before handing it to the application, which
turns streaming silently back into a plain POST; since the backend is ours, that
is a configuration matter (in nginx, `proxy_request_buffering off`).

### The request contract

```
POST {config::kBackendBaseUrl}{config::kAudioPath}      ->  POST /audio
Content-Type: audio/wav

<WAV: PCM, 16 kHz, mono, signed 16-bit little-endian>
```

Sample rate and format travel in the WAV header rather than in custom headers,
so the body is self-describing and can be saved and played back as a file on the
backend side.

The request carries no credentials -- [D8](deferred.md). The backend is on the
LAN and answers anyone who can reach it, which is the same trust boundary the
WiFi password already draws.

The response is JSON with the answer in `response`:

```json
{ "response": "..." }
```

Anything else -- a non-200 status, a body that does not parse, or valid JSON
without a `response` field -- is one error to the device, shown on screen. Richer
error reporting can come later.

### Transport security

Plain HTTP to start -- [D5](deferred.md). HTTPS is wanted, because the backend is meant to live in
the cloud rather than at home, but on a device that wakes from deep sleep for
every question the TLS handshake is paid every single time. Whether that cost is
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
44-byte header should be reserved at the front of the allocation and filled in
once the length is known, so that sending never copies a megabyte to prepend it.

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
start. Measured at 169 ms from the wake event to the first captured sample, of
which 57 ms was the boot itself and 101 ms a latch delay that has since been
taken off the wake path -- [E1](experiments.md).

Caching the BSSID and channel in RTC memory across sleeps and passing them to
`WiFi.begin(ssid, pass, channel, bssid)` cuts reconnect time substantially, and
reconnect time is what the buffer exists to cover.

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

### Concurrency: two tasks

Capture and networking cannot share a thread. The I2S DMA holds 6 x 240 frames,
which is 90 ms at 16 kHz, while a blocking WiFi connect takes seconds. Audio
would be dropped in chunks.

- **Capture task**, pinned to core 1, reads I2S into the buffer and polls the
  button between reads. A `gpio_get_level()` costs nothing and never blocks, and
  it buys a tight stop: the other task's loop refreshes the panel, so a release
  arriving during a refresh would otherwise add a second or two of room noise to
  the end of every recording.
- **Network/UI task** does WiFi, HTTP and the e-paper. The WiFi and lwIP tasks
  sit on core 0, so the recording and the network never share a core either.

E-paper refresh belongs in the second task: it is a long blocking SPI transfer
followed by a BUSY wait, and it must not sit between two I2S reads.

Two tasks are enough while the upload happens after the button is released,
because rendering and uploading never overlap: the "Listening" refresh runs while
WiFi is still associating, and WiFi makes progress in its own IDF tasks
regardless. The chunked variant breaks that and needs a third task
([D4](deferred.md)), so keeping display calls behind a small interface now makes
that split cheap later.

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

Three transitions exist: asleep -> Listening, Listening -> answer, Listening ->
error. All three take a full refresh for now -- [D6](deferred.md).

**Landscape, with the three buttons along the bottom edge on the right.** That
is where the AI button falls under the right thumb, which is the hand the device
is held in. It is also the panel's default orientation in Seeed_GFX2 --
`Board_reTerminal_Sticky` sets a horizontal mirror and no rotation -- so nothing
calls `setRotation()` and the code says nothing about orientation at all. The
other way up reads exactly as well and was tried; it puts the buttons along the
top and off to the left, which is a two-handed device. Checked on the panel both
ways round.

The answer stays on screen until the next question -- that is the point of
e-paper. The Listening screen replaces it on button press, so the previous
answer disappears as soon as a new question starts.

Battery level belongs on the **answer and error screens**, not on the Listening
screen -- read from the BQ27220 fuel gauge on the sensor I2C bus (address
`0x55`, register `0x2C`, two bytes little-endian, percent). No screen shows it
yet: nothing on this unit has talked to that gauge, so it waits for
[E5](experiments.md), which has to establish the same conversation -- [D3](deferred.md).

That placement started as a workaround -- the sensor bus runs over GPIO0, a
strapping pin, so it cannot be touched at the very top of boot -- but it is the
better place anyway: the wake path stays short where latency is felt, and the
figure shown is the one measured after WiFi and the upload have already drawn
their current.

The wider UI is deliberately unconsidered until the proof of concept works.

### Text rendering

Answers can be in Russian, so the display has to render Cyrillic eventually.
Seeed_GFX2's `SmoothFont` loads VLW fonts and looks glyphs up by Unicode code
point, which is the path; nothing else in the library can draw anything outside
ASCII.

Until that is taken on, answers arrive already transliterated, done on the
backend -- [D1](deferred.md).

### Sound

The buzzer on GPIO48 is the only feedback fast enough to be useful -- the e-paper
is one to two seconds behind everything. Three patterns, distinguishable without
looking:

| Event | Pattern |
| --- | --- |
| Ready to listen | two very short notes, low then high |
| Answer received | two short notes, high then low |
| Error | one longer note |

The rising pair opens a question and the falling pair closes it, so the two
normal outcomes are opposites, and the single sustained note is neither.

The ready chirp sounds *after* capture has started, so the microphone records
it. That is the cheaper trade: chirping first and then starting capture would
add the chirp's own duration to the dead time at the front of every question.
The notes are short, and the backend can drop the head of the recording if it
ever matters.

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
error.

If the display itself fails to initialise there is nothing to draw on; that case
chirps, logs to Serial1 and sleeps.

Two cases are not errors:

- **Press shorter than `kButtonMinHoldMs`.** An accidental tap. Nothing is sent
  and nothing is shown -- the device goes straight back to sleep.
- **Recording reached the 30 s cap.** Capture stops and whatever was recorded is
  sent as a normal question.

### Configuration and secrets

Split in two, by whether a value can be committed:

- **`src/config.h`** -- tracked. Backend base URL and path, recording cap,
  response timeout, button debounce and minimum hold. Meant to be edited.
- **`src/secrets.h`** -- gitignored. WiFi credentials, and whatever else turns
  out not to be committable.
  `src/secrets.example.h` is the committed template and must be kept in step
  when a constant is added. A missing `secrets.h` breaks the build at the
  include; empty values are reported over Serial1 at runtime.

Secrets are plain strings in the firmware image and anyone who can read the
flash can read them. Acceptable for a personal device; they should not be
credentials that matter elsewhere.

The backend URL sits in `config.h` because it is currently a LAN address. If the
backend moves to a public host whose address is worth not publishing, it moves
to `secrets.h`.

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
`src/sticky_epaper.h` and `src/sticky_mic.cpp` for the details; the short
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
  2/4/6/7/8 are unavailable. Rendering a paragraph of answer text will need a
  FreeFont via `setFreeFont()` plus word wrapping.
- Sources disagree on whether the Sticky has microSD enable and detect pins.
  Seeed's firmware declares GPIO10 and GPIO11; Seeed_GFX2 states the slot has
  neither. Unresolved, and irrelevant until the SD card is used.

### Measured on this unit

- Quiet room reads about -70 dBFS; speech at arm's length peaks near -45 dBFS.
- The ESP32-S3 PDM-to-PCM path has no high-pass stage, so raw samples carry the
  microphone's DC bias. It has to be removed per block or silence does not read
  as silence.

## What is still moving

Nothing about the design is currently unresolved. What remains is tracked
elsewhere, deliberately kept out of this document so it does not age every time
a shortcut is taken, a number comes in or a step is finished:

- **[deferred.md](deferred.md)** -- simplifications taken on purpose, each with
  the end state it stands in for and what triggers the change.
- **[experiments.md](experiments.md)** -- measurements the design is waiting on:
  wake latency, HTTPS overhead, microphone settle time, and whether the AI button
  reacts to a long hold in hardware.
- **[implementation.md](implementation.md)** -- the order the firmware is being
  built in, and what each step turned out to involve.
