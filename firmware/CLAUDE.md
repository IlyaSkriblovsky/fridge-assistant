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
| `src/main.cpp` | Boot sequence, power latch, display and WiFi setup |
| `src/sticky_mic.h/.cpp` | PDM microphone: power, I2S PDM-RX, level measurement |
| `src/sticky_epaper.h` | `Driver_SSD1677_Sticky` -- two corrections to Seeed_GFX2's SSD1677 path |
| `src/config.h` | Tracked settings: backend URL, timeouts, button thresholds |
| `src/secrets.h` | Credentials. Gitignored. Template: `src/secrets.example.h` |
| `docs/project-vision.md` | The idea, the decisions, the pin map, the traps |
| `docs/experiments.md` | Measurements still owed, and what each one unblocks |
| `platformio.ini` | Pinned Seeed_GFX2 and platform revisions |

## Build and flash

`pio` is not on PATH:

```
~/.platformio/penv/bin/pio run                                  # build
~/.platformio/penv/bin/pio run -t upload --upload-port <port>    # flash
```

The board enumerates as a CH343P bridge (`1A86:55D3`), typically
`/dev/cu.usbmodem*`. **Logging is `Serial1` on UART0 (GPIO43/44), not `Serial`.**
`Serial` is the native USB CDC, whose pins the microphone takes over.

A PlatformIO monitor left running in the user's editor holds the port and makes
uploads fail with "port is busy"; ask the user to close it rather than killing
their process.

## Things that will bite

- **Never drop the power latch.** GPIO45/46 high, first thing in `setup()`.
  Without it the board cuts its own rail. It works fine on USB, so this only
  shows up on battery.
- **Don't touch GPIO19/20 for anything else.** They are the microphone, and they
  are also the native USB-Serial-JTAG pads that `StickyMic::begin()` has to take
  away from the USB PHY.
- **Don't "simplify" `src/sticky_epaper.h`.** Both overrides work around real
  library bugs -- inverted polarity and a missing previous-image plane on partial
  refresh. Removing either brings back an inverted or smearing display.
- **Don't edit `.pio/libdeps/`.** It is wiped by package updates. Library fixes
  belong in `src/` as subclasses, which is what `sticky_epaper.h` does.
- **Capture and networking must not share a task.** The I2S DMA holds only 90 ms;
  a blocking WiFi connect drops audio.
- **Keep `src/secrets.example.h` in step with `src/secrets.h`,** and never put
  real credentials in a tracked file. Settings that are not secret belong in
  `src/config.h`, which is tracked.
- **Don't guess at a number that belongs in `docs/experiments.md`.** Wake
  latency, HTTPS overhead and microphone settle time are unmeasured on purpose;
  record a measurement there rather than inventing a constant.

## Verifying on hardware

There is a device on the desk and changes to display or microphone behaviour
should be flashed and checked. Serial output confirms the microphone; the screen
has to be checked by the user, so ask.
