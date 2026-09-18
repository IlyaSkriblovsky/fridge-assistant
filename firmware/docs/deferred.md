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
| D4 | Whole recording POSTed after release | Chunked streaming upload | Latency proving to matter -- [S7](implementation.md#s7----upload-and-answer) says it has for long questions, and [E7](experiments.md) says the stalls move under the hold with the bytes. Scheduled as [S11](implementation.md#s11----streaming-upload), after [S10](implementation.md#s10----display-task) |
| D5 | Plain HTTP | HTTPS | [E2](experiments.md) |
| D6 | Everything partial but the Listening screen, which is full because nothing survives the sleep to be differential against | The same waveforms, with the refresh off the orchestrator's thread | Paid at [S8](implementation.md#s8----the-flow); the thread is [S10](implementation.md#s10----display-task) |
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

Two things had to be settled before this could change, and both now are:

- **The body stays a WAV, with `0xFFFFFFFF` in both length fields.** A WAV
  header declares a length that is unknown when a chunked request opens, and the
  transport carries it instead: the terminating chunk is where the body ends, so
  the backend takes the length from there and rewrites or strips the header as
  it needs. `0xFFFFFFFF` rather than zero because Python's `wave`, which the
  prototype backend reads its uploads with, takes the first as a file of unknown
  length and reads every sample in it, and refuses the second outright as `not a
  WAVE file` -- checked with a one-second tone, both ways.

  Raw PCM with the format in request headers was the other way, and it buys
  nothing. The header would only move to the backend, which needs the format
  either way -- to save a file that plays now, and to hand the audio on later --
  and there is no standard type for little-endian PCM to carry it in:
  `audio/L16` is big-endian by RFC 2586. So it would mean swapping a megabyte on
  the device or headers of our own, and the second is the bespoke protocol the
  vision's transport decision exists to avoid.
- **Display gets its own task**, and it is
  [S10](implementation.md#s10----display-task), ahead of this entry's own
  [S11](implementation.md#s11----streaming-upload). With a single POST after
  release, rendering and uploading never overlap; with a streaming upload, a
  one-to-two second panel refresh would stall it -- and since S7b that refresh is
  the *only* thing between the wake and a network that is ready to take bytes.

**S8 has put a number on what the task is worth, and it is not the upload.**
The working screen is 990 ms on this thread and the leftover Listening refresh
is up to 2 s more -- so on a cached wake with a quick backend the panel is the
majority of what the user waits through, and every millisecond of it is a
millisecond a display task removes. That is about a second a question on a long
hold and closer to two and a half on a short one, before the streaming upload
this entry is nominally about saves anything at all. It is also the precondition
for the last part of [D6](deferred.md), whose full refresh has to run after the
answer while the orchestrator is going to sleep.

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

**Most of this is paid.** [S8](implementation.md#s8----the-flow) made the
working screen, the answer and the errors partial, and the two things this entry
said were owed first are now known: a partial takes **1344 ms over the whole
panel** and 990 ms over the word's 136 rows, against 2400 ms for a full refresh
either way, deterministic to the millisecond over fourteen of them -- and the
panel comes out clean over a run of consecutive questions, checked by eye. The
partial-refresh correction in `src/sticky/epaper.h`, written at S5 and never
exercised, has run.

The saving is 1056 ms a transition and it does not shrink with area: two window
sizes were enough to separate the fixed cost from the per-row one and show that
the whole difference is waveform, not transfer. The numbers are in S8.

**What is left is one transition, the Listening screen, and it stays full.** It
has to be: a partial update is differential against what the controller has been
told is on the glass, and after a deep sleep the firmware has been told nothing
while the panel still holds whatever the last wake left. Draw `LISTENING`
partially in that state and the old image's black pixels stay exactly where they
are with the word on top. Every other transition is safe precisely because this
one ran first.

**And it cannot be argued out of.** The way round would be for the wake to
reconstruct what is on the glass and seed the shadow in `epaper.h` from it
without touching the panel -- which needs the previous image to be something the
firmware can rebuild from what survives the sleep. It is not. The screen left on
the glass between questions is meant to be an idle screen of the wider UI's own,
and its content is not expected to be reconstructible after a wake: whatever it
shows, it will have been built from things the device had while it was awake.
Keeping a 48000-byte frame in RTC memory is not an option either -- there are
8192 bytes of it, and the WiFi lease is already in them.

So the arrangement S8 arrived at is the end state for the waveforms: **one full
refresh a wake, and it is `LISTENING`.** What is still owed is not a cheaper
refresh but a thread to run it on, and that is
[S10](implementation.md#s10----display-task).

**The panel comes off the critical path through [D4](deferred.md)'s display
task, not through a partial.** That is the correction this entry needed: the
2.4 s of `LISTENING` is only in the way because the orchestrator sits inside it
and therefore cannot notice the button coming up -- 1239 to 1809 ms of it left
over on a short question, measured in S8. Behind a queue it stops being in the
way at all. The orchestrator posts the screen, keeps polling capture, sees the
release when it happens, chirps, and starts the upload while the panel is still
catching up. The wait stops containing any panel at all and becomes the round
trip, which on a cached wake with a quick backend is 282 ms against the 3166 ms
S8 measured on the same question.

Two things that arrangement wants, neither of them hard and both worth writing
down before someone builds it:

- **Nothing may sleep with the queue unfinished.** Deep sleep would cut a
  refresh in half, so the exit waits for the display task to drain. That is
  awake time rather than wait -- it is all past the answer chirp -- but it is
  the reason the two are worth keeping apart in the log.
- **A superseded screen should be dropped rather than drawn.** If the answer
  lands before `WORKING` has started, drawing `WORKING` costs a second and shows
  the user a word that was already stale when it appeared. A queue of one with
  replacement is probably the whole of it.

**The ghosting depth stays fixed at two either way**, which is worth stating
because an earlier draft of this entry had it as an open question. Full,
partial, partial -- and, once there is an idle screen, a second full refresh
before sleep -- so between any two full refreshes there are never more than two
partials, however many questions are asked. Nothing accumulates across a run.
"How many partials does this panel take" would only have become a question if
the full refresh had moved off the wake, and it is not moving.

**The one thing an idle screen does settle** is how well a partial image keeps.
The answer screen is drawn partially and a partial waveform drives pixels less
hard, so if the answer were the image that lived on the glass between questions
-- possibly for days -- how well it holds would be worth measuring. With an idle
screen replacing it after some seconds, and that idle screen drawn by a full
refresh, the image that has to survive the night is always fully driven and
there is nothing to measure. That is a reason to want the idle screen that has
nothing to do with the UI.

What it costs is awake time, and more of it than it first looks: two full
refreshes a cycle rather than one, plus the five or ten seconds of waiting in
between, on a device currently awake six to twelve seconds a question. Whether
that is affordable waits for [E5](experiments.md), which is what would say what
a second awake is worth against a night asleep.

The rest waits for the UI/UX pass, with the other display work -- and for
[D4](deferred.md)'s display task, since a refresh that happens after the answer
has to happen while the orchestrator is on its way to sleep.

## D7 -- Nothing for a press too short

A press shorter than `config::kButtonMinHoldMs` is discarded silently: no
screen, no sound, nothing sent.

"No sound" cannot be quite true, because the ready chirp comes first by design.
The chirp sounds 104 ms after the wake, with capture already running --
[E1](experiments.md), re-run on the firmware at
[S9](implementation.md#s9----re-run-e1) -- while the minimum hold is 300 ms. So a tap between the
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
