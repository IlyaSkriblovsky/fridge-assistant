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
| D3 | Battery ignored entirely | Level on the answer and error screens, then some low-battery behaviour | [E5](experiments.md), which has to talk to the gauge anyway |
| D4 | Whole recording POSTed after release | Chunked streaming upload | Latency proving to matter -- [S7](implementation.md#s7----upload-and-answer) says it has for long questions, and [E7](experiments.md) says the stalls move under the hold with the bytes |
| D5 | Plain HTTP | HTTPS | [E2](experiments.md) |
| D6 | Full refresh on every transition | Partial refresh where it pays | The working screen at S8, or the UI/UX pass |
| D7 | A press too short to count makes no sound | Some feedback | The UI/UX pass |
| D8 | The request carries no credentials | Some device authentication | The backend leaving the LAN, with [D5](deferred.md) |
| D9 | Hold to talk, release to send | An interaction that does not require holding | The UI/UX pass |

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

## D3 -- No battery reading

The screens show no battery level, and the gauge is never read. That is one step
further back than it looks: nothing on this unit has ever talked to the BQ27220,
so the first version would have had to bring up an I2C bus on a strapping pin,
confirm the device answers at `0x55`, and decide what a reading from a gauge
that has never learned this pack is worth -- all inside the one path where a
failure costs the user their answer.

[E5](experiments.md) needs exactly that conversation for its own reasons, and it
can have it where a wrong answer costs nothing. So the gauge gets proven there
first, and the answer screen picks it up afterwards.

Acting on a low reading stays a separate question after that, and an open one.

## D4 -- Chunked upload

The recording is sent in one POST after the button is released, which is the
simplest thing that proves the chain end to end. It costs the whole utterance
plus the upload before the backend sees anything.

**The trigger has half fired.** [S7](implementation.md#s7----upload-and-answer)
measured the upload on the device and it is the largest term the device controls:
400428 bytes took 2876 ms and 331308 took 4782 ms, all of it after the button
came up, against a backend that swallows the same bytes in 4 ms. Streaming would
move nearly all of that under the recording, where the user is still talking and
it costs nothing.

One thing kept it from being obvious and [E7](experiments.md) has now settled
it, in this step's favour:

- **It bought nothing for a short question, and that has changed.** The
  objection was that the network is not usable until about 3.4 s after the
  press, so a one or two second recording has no window to stream into and would
  still send everything after the release.
  [S7b](implementation.md#s7b----cached-dhcp-lease) has removed the premise: a
  cached lease puts the device on a usable network 300 ms after the wake, so
  every question has a window now and the saving no longer scales only with
  length. What is in the way instead is the second point below -- the panel
  refresh holds the orchestrator's thread for 2.4 s of that window -- which
  makes the display task a precondition for this step rather than a tidying-up
  afterwards.
- **Part of the upload is not upload, and it is spread through the body.** The
  same run showed 112684 bytes taking 6848 ms while three times that went in
  half the time, and [E7](experiments.md) found what those seconds are: the
  device keeps 5744 bytes in flight, one ACK in 110 is lost coming back, and
  with nothing behind it to cover the loss the window stops for a full
  retransmission timeout of one to two and a half seconds. An upload is
  twenty-two windows, so about one question in four pays -- anywhere in the
  body, which is the case that makes this step worth the most. Streaming moves
  the stalls under the hold along with the bytes. It also scales the right way
  for once: a long question has more windows and therefore more chances to
  stall, and a long question is exactly the one with room to stream.

Two things have to be settled when this changes:

- A WAV header declares a length that is unknown when a chunked request opens.
  Either the length fields get a placeholder the backend agrees to ignore, or the
  body switches to raw PCM with the format moved into request headers.
- Display gets its own task. With a single POST after release, rendering and
  uploading never overlap; with a streaming upload, a one-to-two second panel
  refresh would stall it -- and since S7b that refresh is the *only* thing
  between the wake and a network that is ready to take bytes.

**That task is close to free, which was not obvious.** A full refresh takes
2.4 s on this panel ([S5](implementation.md#s5----screens)), but almost none of
it is CPU: the library waits for the controller on the BUSY pin in a
`delay(1)` + `yield()` loop (`core/Gpio.h`, `gfxWaitForPin`), and `delay()` in
arduino-esp32 is `vTaskDelay()` against a 1000 Hz tick. So a refresh is about
2200 yields, and a task doing nothing but drawing is blocked for essentially all
of its life -- it can share a core with anything and wants no priority to speak
of.

The same fact explains a result S5 and [S4](implementation.md#s4----capture-task)
both measured: the panel costs the capture task nothing. While the orchestrator
is inside a refresh it is parked in `vTaskDelay`, so the capture task at
priority 10 preempts it freely rather than queueing behind it.

## D6 -- Partial refresh

All three transitions -- asleep to Listening, Listening to answer, Listening to
error -- use a full refresh. Each changes most of the screen, and a full refresh
clears accumulated ghosting as a side effect.

This means the partial-refresh correction in `src/sticky/epaper.h` is currently
unused -- it has never run on the device at all. It stays: the library bug it
works around returns the moment anything draws a partial update, and a correct
driver is worth more than a smaller one.

**The working screen may pull this forward.** The vision's step 6 is the one
transition on the critical path of a question, so it is the first place where
2.4 s is spent out of the user's time rather than under a recording -- see
[S8](implementation.md#s8----the-flow). If that is what a partial refresh is
for, two things are owed first: how long one takes on this panel, which nobody
has measured, and what the accumulated ghosting looks like over a real sequence
of transitions, which only a human at the panel can say.

## D7 -- Nothing for a press too short

A press shorter than `config::kButtonMinHoldMs` is discarded silently: no
screen, no sound, nothing sent.

"No sound" cannot be quite true, because the ready chirp comes first by design.
Capture starts roughly 70 ms after the wake -- [E1](experiments.md)'s 169 ms
less the latch delay that has since come off the wake path -- and the chirp
follows it immediately, while the minimum hold is 300 ms. So a tap between the
two has already been answered with "the microphone is live", which was true when
it sounded. Nothing further happens.

Both alternatives are worse than the inconsistency: waiting out the minimum hold
before chirping puts 300 ms of dead time at the front of every question, and a
minimum hold shorter than the chirp is not a minimum hold.

Feedback of its own for the discarded tap waits for the UI/UX pass -- along with
[D9](deferred.md), which may remove the case entirely.

## D8 -- No authentication

The POST carries no `Authorization` header and the backend does not ask for one.

Nothing is protected by this that the WiFi password does not already protect:
the backend is a LAN address, so reaching it means already being on the network.
A token in the firmware image would not change that -- it is readable by anyone
who can read the flash, which is anyone holding the device.

It becomes real the day the backend moves to a public host, which is the same
day TLS does ([D5](deferred.md)) -- and a bearer token sent in clear would be
worse than none, so the two arrive together or not at all.

## D9 -- Press and hold

The device records while the AI button is held and sends on release. It is the
simplest interaction that has a beginning and an end, and the only one that
needs no way of guessing when the user has finished talking.

That is not the same as it being right. A short press to start and a second
press to stop would let the user put the device down mid-sentence; so would a
tap on the screen, or simply a run of silence. Each needs something this one
does not -- silence detection needs a threshold and a hangover time, the touch
panel is unpowered and unused, and a start/stop pair needs the device awake and
listening in between -- and none of them is obviously better without a working
device to try them on.

Cheap to defer, because all of them change only *when the recording stops*.
Everything else in the pipeline is the same either way, so the question can wait
for the UI/UX pass and take [D7](deferred.md) with it.
