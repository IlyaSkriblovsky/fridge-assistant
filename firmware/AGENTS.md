# reTerminal Sticky voice assistant

Push-to-talk voice assistant on a Seeed reTerminal Sticky (E1005): ESP32-S3,
3.97" 800x480 mono e-paper, PDM microphone, battery. Hold the AI button, speak,
release; the audio goes to our backend and the answer is drawn on the e-paper.

**Read `docs/project-vision.md` before changing anything structural.** It holds
the interaction flow, the decisions and why they were made, the full pin map and
the hardware traps.

## Where things are

| Path | What |
| --- | --- |
| `src/main.cpp` | The orchestrator: wake, record, ask, show, sleep |
| `src/sticky/power.h/.cpp` | Power latch, deep sleep entry, wake reporting |
| `src/sticky/buzzer.h/.cpp` | Buzzer on LEDC: the ready, taken, answer and error patterns |
| `src/sticky/button.h/.cpp` | AI button: the press that woke the board, timed and debounced |
| `src/sticky/mic.h/.cpp` | PDM microphone: power, I2S PDM-RX, level measurement |
| `src/sticky/screen.h/.cpp` | The screens, and the only place the panel is touched; only `Display` calls them |
| `src/sticky/epaper.h` | `Driver_SSD1677_Sticky` -- two corrections to Seeed_GFX2's SSD1677 path |
| `src/recording.h/.cpp` | The recording: one PSRAM buffer that is already a WAV |
| `src/capture.h/.cpp` | The capture task: I2S reads and the button poll, off the orchestrator's thread |
| `src/display.h/.cpp` | The display task: owns the screen, draws what the orchestrator posts, off its thread |
| `src/text.h/.cpp` | Laying UTF-8 out in a face made of several GFXfonts, because the library cannot |
| `src/fonts/` | The four faces, generated. Latin, Greek, Cyrillic and the punctuation those bring |
| `src/wifi_link.h/.cpp` | The association: polled, never waited on, with the AP and the DHCP lease cached across sleeps |
| `src/dashboard.h/.cpp`, `src/dashboard_protocol.h` | Background frame download, validation and interval rules |
| `src/backend.h/.cpp` | The request: the recording streamed up as a chunked POST while the button is held, the answer back as JSON |
| `src/silent_mode.h/.cpp` | NVS sound preference, loaded before buzzer/display work |
| `src/silent_icon.h` | Reserved top-margin indicator geometry, shared with the preview |
| `src/config.h` | Tracked settings: the backend path, timeouts, button thresholds |
| `src/secrets.h` | WiFi credentials, an optional DNS server, the backend's base URL and token. Gitignored. Template: `src/secrets.example.h` |
| `src/experiments/` | Measurement rigs, one per experiment, each its own PlatformIO env |
| `tools/` | The host half of an experiment: the serial logger, the pcap reader E7 needs |
| `tools/gfxfont.py` | Regenerates `src/fonts/` from the GNU FreeFont TTFs. Its docstring is the format |
| `docs/project-vision.md` | The idea, the decisions, the pin map, the traps |
| `docs/deferred.md` | Shortcuts taken on purpose, and what each stands in for |
| `docs/experiments.md` | Measurements still owed, and what each one unblocks |
| `docs/implementation.md` | The build order, step by step, and notes from each step |
| `platformio.ini` | Pinned Seeed_GFX2 and platform revisions, plus ArduinoJson |

`src/sticky/` is the board: a module belongs there when it cannot be read
without the pin map or cannot be checked without the device. The folder carries
that fact, which is why the files inside it are named plainly -- `sticky/mic.h`
declares `StickyMic`, and the include path is where the prefix is spelled.
Everything at the top of `src/` is logic that happens to run here: it may know
about PSRAM, FreeRTOS or arduino-esp32, but not about the E1005. A new module
goes into `sticky/` only if it touches the hardware.

The word means the reTerminal Sticky, not this project. The project has no name
yet, and the directory it is checked out into is not one.

## Build and flash

`pio` is not on PATH:

```
~/.platformio/penv/bin/pio run                                  # build
~/.platformio/penv/bin/pio run -t upload --upload-port <port>    # flash
~/.platformio/penv/bin/pio run -e exp_e1 -t upload ...           # an experiment rig
~/.platformio/penv/bin/pio run -e exp_e6 -t upload ...           # ... one per experiment
```

The board enumerates as a CH343P bridge (`1A86:55D3`), typically
`/dev/cu.usbmodem*`. **Logging is `Serial1` on UART0 (GPIO43/44), not `Serial`.**
`Serial` is the native USB CDC, whose pins the microphone takes over.

A PlatformIO monitor left running in the user's editor holds the port and makes
uploads fail with "port is busy"; ask the user to close it rather than killing
their process.

CI (`../.github/workflows/firmware.yml`) builds every embedded `[env:...]` in
`platformio.ini`, a new rig included, with `src/secrets.example.h` copied in as
`src/secrets.h`. So every environment has to build without real credentials.
CI also runs all host tests with `pio test -e native`; see `test/README`.
The native environment
is excluded from the embedded build job. Nothing runs on hardware in CI.

## Things that will bite

- **Never drop the power latch.** GPIO45/46 high, first thing in `setup()`.
  Without it the board cuts its own rail. It works fine on USB, so this only
  shows up on battery.
- **Don't touch GPIO19/20 for anything else.** They are the microphone, and they
  are also the native USB-Serial-JTAG pads that `StickyMic::begin()` has to take
  away from the USB PHY.
- **Don't "simplify" `src/sticky/epaper.h`.** Two of its overrides work around
  real library bugs -- inverted polarity and a missing previous-image plane on
  partial refresh. Removing either brings back an inverted or smearing display.
  The third, `sleep()`, drops a `delay(100)` nothing waits on; removing it only
  puts 100 ms back on every screen, measured in [E8](docs/experiments.md).
- **An SSD1677 RAM window does not limit the optical refresh.** After panel
  power loss, reconstructing only the icon's old pixels leaves random RAM
  outside it. Seed both full controller planes identically before applying the
  window transition; keep the shadow-prime initialization in `sticky/epaper.h`.
  `pio test -e native -f test_epaper` checks the RAM invariant on the host.
- **Don't edit `.pio/libdeps/`.** It is wiped by package updates. Library fixes
  belong in `src/` as subclasses, which is what `src/sticky/epaper.h` does.
- **Never draw text with `drawString()` or measure it with `textWidth()`.**
  Seeed_GFX2's are byte-oriented -- its GFXFF branch is `uniCode = *(string++)`
  with no UTF-8 decode -- so every Cyrillic or Greek string measures as empty
  and anything centred on that measurement lands wrong. Neither function is
  virtual, so this cannot be subclassed away. `src/text.h` is the replacement,
  and it is the only text path. The same applies to a new screen: measure with
  `textWidth(face, ...)` and draw with `textDraw()`.
- **A face's bitmaps have to stay under 64 KiB.** `drawCharGfx()` reads
  `GFXglyph::bitmapOffset` with `pgm_read_word` although the struct field is a
  `uint32_t`, so a larger font draws glyphs from the wrong place with no error.
  Each range in `src/fonts/` has its own array and the largest is about 8 KB,
  but a face added at 48 pt would not be. `tools/gfxfont.py` refuses to emit
  one that would.
- **Regenerate all faces together, never one.** A newer FreeType
  rasterises a handful of edge pixels differently from the one Adafruit used,
  so mixing a generated face with a bundled GFXFF header puts two rasterisers
  in one string. `tools/gfxfont.py` emits the whole set for that reason.
- **Capture and networking must not share a task.** The I2S DMA holds only 90 ms;
  a blocking WiFi connect drops audio.
- **A number read off a poll is a number about the poller.** The orchestrator
  sits inside a 2.4 s panel refresh between one `wifi.poll()` and the next, so
  anything timed by when a poll noticed has the panel in it -- S7b's first run
  reported an address that arrives in 40 ms as costing 2642 ms, four wakes
  running. `WifiLink::linkMs()` and `onlineMs()` are timestamps from the WiFi
  task for that reason; `elapsedMs()` is deliberately the polling thread's own
  view. Time anything else that happens off this thread the same way.
  `Backend::firstByteUs()` is the same trap from the other side: it is read
  after the taken chirp, so a backend quicker than 60 ms reads as the chirp.
- **The request opens on `Capture::pastMinimumHold()`, not on the clock.** A
  release is confirmed a debounce window after it happens, so the clock says
  "past the minimum hold" about taps that are already over, and a tap must
  never upload audio. It may fetch the idle dashboard afterwards.
- **Only the capture task commits to `Recording`.** The upload reads the buffer
  while it grows; the published count is the one thing the two share, and it is
  what makes that safe without a lock.
- **`WiFi.persistent(false)` goes before `WiFi.mode()`.** Arduino's default
  storage is FLASH, so a mode set before that call goes through NVS and costs
  1.6 s on every wake -- measured in [E6](docs/experiments.md). `WifiLink`
  does it in the right order; anything else touching WiFi has to as well.
- **Keep `src/secrets.example.h` in step with `src/secrets.h`,** and never put
  real credentials in a tracked file. Settings that are not secret belong in
  `src/config.h`, which is tracked.
- **Put a shortcut in `docs/deferred.md`, not in the vision.** The vision says
  what the device is; deferrals are the list that gets deleted a line at a time.
- **Work through `docs/implementation.md` and leave the notes there.** It holds
  the step order and what each step turned out to involve. A step counts as done
  when it has run on the device, not when it builds.
- **Don't guess at a number that belongs in `docs/experiments.md`.** Wake
  latency, HTTPS overhead and microphone settle time are unmeasured on purpose;
  record a measurement there rather than inventing a constant.

## Verifying on hardware

There is a device on the desk and changes to display or microphone behaviour
should be flashed and checked. Serial output confirms the microphone; the screen
has to be checked by the user, so ask.
