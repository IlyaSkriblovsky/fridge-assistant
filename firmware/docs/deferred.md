# Deferred

Simplifications taken on purpose to get a proof of concept working, each with
the end state it stands in for. None of them is an argument about what the
device should be -- that is [project-vision.md](project-vision.md). This is the
list that gets deleted a line at a time.

Kept separate for two reasons: the vision should not age every time a shortcut
is taken, and when a shortcut is finally paid off it helps to find the reasoning
in one place rather than archaeology through commit messages.

| # | For now | End state | What triggers the change |
| --- | --- | --- | --- |
| D1 | Answers transliterated to ASCII on the backend | Cyrillic rendered on the device | Taking on fonts |
| D2 | Answers assumed short enough to fit, drawn as-is | Word wrap and pagination | The UI/UX pass |
| D3 | Battery level shown, nothing acted on | Some low-battery behaviour | Undecided |
| D4 | Whole recording POSTed after release | Chunked streaming upload | Latency proving to matter |
| D5 | Plain HTTP | HTTPS | [E2](experiments.md) |
| D6 | Full refresh on every transition | Partial refresh where it pays | The UI/UX pass |
| D7 | A press too short to count makes no sound | Some feedback | The UI/UX pass |

---

## D1 -- Cyrillic on the display

Answers can be in Russian and the display cannot draw Cyrillic at all. The GFXFF
FreeFonts declare the range `0x20`-`0x7E`, the built-in GLCD font is ASCII, and
`font/Custom` holds only Latin display faces.

The real answer is Seeed_GFX2's `SmoothFont`, which loads VLW fonts and looks
glyphs up by Unicode code point. Two things make it a job rather than a switch:
it is a separate drawing API from `drawString`, and it renders with alpha
blending, so on a 1bpp panel the intermediate levels need thresholding.

Until then the backend transliterates. Putting it there rather than in firmware
means the day the device can render Cyrillic, this is a server-side switch and
not a reflash. The device should still degrade gracefully if a non-ASCII byte
arrives rather than drawing garbage.

This costs nothing on the parsing side either way: ArduinoJson decodes `\uXXXX`
escapes, surrogate pairs included, into UTF-8 by itself.

## D4 -- Chunked upload

The recording is sent in one POST after the button is released, which is the
simplest thing that proves the chain end to end. It costs the whole utterance
plus the upload before the backend sees anything.

Two things have to be settled when this changes:

- A WAV header declares a length that is unknown when a chunked request opens.
  Either the length fields get a placeholder the backend agrees to ignore, or the
  body switches to raw PCM with the format moved into request headers.
- Display gets its own task. With a single POST after release, rendering and
  uploading never overlap; with a streaming upload, a one-to-two second panel
  refresh would stall it.

## D6 -- Partial refresh

All three transitions -- asleep to Listening, Listening to answer, Listening to
error -- use a full refresh. Each changes most of the screen, and a full refresh
clears accumulated ghosting as a side effect.

This means the partial-refresh correction in `src/sticky_epaper.h` is currently
unused. It stays: the library bug it works around returns the moment anything
draws a partial update, and a correct driver is worth more than a smaller one.
