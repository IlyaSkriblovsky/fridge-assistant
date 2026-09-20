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
| D2 | Answers assumed short enough to fit, drawn as-is | Word wrap and pagination | The UI/UX pass -- felt harder since D1 went |
| D3 | Battery ignored entirely | Level on the answer and error screens, then some low-battery behaviour | [E5](experiments.md), which has to talk to the gauge anyway |
| D5 | Plain HTTP, the device's token included | HTTPS | [E2](experiments.md) |
| D7 | A press too short to count makes no sound | Some feedback | The UI/UX pass |
| D9 | Hold to talk, release to send | An interaction that does not require holding | The UI/UX pass |

---

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
