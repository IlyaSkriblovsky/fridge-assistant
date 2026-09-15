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
2. **Press.** Wake, hold the power latch, chirp the buzzer immediately -- that
   chirp is the "speak now" cue, not the screen.
3. **Record.** Power the microphone, capture 16 kHz mono PCM into a linear
   buffer in PSRAM. Draw a "Listening" screen whenever the panel gets round to
   it; it will be late and that is accepted.
4. **Connect.** Bring up WiFi concurrently with recording, in a separate task.
5. **Release.** Debounce, then stop capturing.
6. **Upload.** POST the buffer to the backend.
7. **Answer.** The backend replies with text. Render it on the e-paper.
8. **Sleep.** Back to deep sleep with the latch held.

## Decisions

### Transport: HTTP POST, not WebSocket

A WebSocket buys streaming at the cost of a library, frame handling, a bespoke
envelope protocol, pings and reconnect logic -- all for a single request and a
single response per session. It is not worth it here. `esp_websocket_client` is
not even shipped with the Arduino framework, so it would be an added dependency.

**Now:** POST the whole recording once the button is released. Simplest thing
that proves the chain end to end. Latency cost is the utterance duration plus
the upload, paid before the backend sees anything.

**Later:** the same POST with `Transfer-Encoding: chunked`. The request opens on
button press, the body streams as audio is captured, and the terminating chunk
*is* the "transmission over" signal -- no separate protocol needed. The response
body is still just the answer. This keeps one request and one response while
letting the backend start transcribing while the user is still talking.

Two things to know before doing that:

- `esp_http_client` supports chunked request bodies (`esp_http_client_open()`
  with a negative length) and is available from the Arduino framework. Arduino's
  own `HTTPClient` only sends bodies of known length, so the chunked path must
  use the IDF client or hand-written requests over `NetworkClient`.
- Servers and proxies commonly buffer a whole chunked request body before
  handing it to the application, which silently turns streaming back into a
  plain POST. Since the backend is ours, this is a configuration matter (in
  nginx, `proxy_request_buffering off`).

### Audio buffer: linear, capped at 30 seconds

PSRAM makes a ring buffer unnecessary -- there is no memory ceiling to slide
against. A linear buffer with a hard cap is less code and avoids reasoning about
a write pointer overtaking a drain pointer.

16 kHz, 16-bit, mono is 32 KB/s, so the 30 s cap is 960 KB. Recording stops at
the cap whether or not the button is still held.

Format on the wire is raw little-endian signed 16-bit PCM. It must be stated in
the request contract (header, query or content type) rather than assumed. WAV is
a poor fit for the chunked variant because its header declares a length that is
not known when the request opens.

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
start. The exact wake-to-first-sample time has not been measured yet.

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

- **Capture task**, pinned to a core, does nothing but read I2S into the buffer.
- **Network/UI task** does WiFi, HTTP and the e-paper.

E-paper refresh belongs in the second task: it is a long blocking SPI transfer
followed by a BUSY wait, and it must not sit between two I2S reads.

### Buttons

Active low with internal pull-ups. The AI button is GPIO4, the side buttons are
GPIO5 and GPIO6.

Debouncing matters on release as well as on press: without requiring a stable
high for 30-50 ms, a bounce ends the utterance mid-sentence. A minimum hold of
roughly 300 ms should also be required so an accidental tap does not wake the
whole pipeline.

### Secrets

`src/secrets.h` holds WiFi credentials and the backend URL and token. It is
gitignored; `src/secrets.example.h` is the committed template and must be kept
in step when a constant is added. A missing `secrets.h` breaks the build at the
include; empty values are reported over Serial1 at runtime.

They are plain strings in the firmware image and anyone who can read the flash
can read them. Acceptable for a personal device; they should not be credentials
that matter elsewhere.

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

## Open questions

- Wake-to-first-sample latency has not been measured. It decides how much of the
  first word is lost and whether light sleep is worth its higher idle draw.
- Answer rendering: word wrap, font choice, and what to do with a reply longer
  than one screen.
- Error paths -- no WiFi, no backend, backend error, timeout -- each needs a
  screen, and every one of them must still end in deep sleep.
- Backend think time is seconds. Read timeouts must allow for it, and idle
  connections can be dropped by NAT in between.
