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
| D2 | An answer longer than a panel ends in an ellipsis | The rest of it reachable -- pagination, or a smaller face | The UI/UX pass |
| D3 | Gauge percentage not yet validated against this pack | Validate discharge readings and decide low-battery behaviour | [E5](experiments.md) |
| D5 | Plain HTTP, the device's token included | HTTPS | [E2](experiments.md) |
| D7 | A press too short to count makes no sound | Some feedback | The UI/UX pass |
| D9 | Hold to talk, release to send | An interaction that does not require holding | The UI/UX pass |
| D10 | Dashboard sends a 48000-byte frame without compression | Decide whether and how to compress real dashboard frames | Dashboard design is chosen; [E10](experiments.md#e10----dashboard-compression-deferred) |
| D11 | Post-answer wait stays awake for 10 seconds with WiFi retained | Compare its energy with sleeping and reconnecting | Dashboard cycle works; [E9](experiments.md#e9----dashboard-energy-deferred) |

---

## D2 -- An answer longer than the panel

The answer wraps as of [S14](implementation.md#s14----word-wrap) -- that half
of D2 is paid off. What is left is the answer that does not fit even wrapped:
seven lines of 24 pt, which is about 375 bytes of Russian. It is drawn to the
last line the panel has room for and that line ends in an ellipsis, and the
rest is not reachable from the device at all.

The ellipsis has only ever been seen in the host preview. The assistant is
asked for a sentence or two and gives them -- three lines was the longest
answer of S14's run on the device -- so nothing it says in normal use reaches
the bottom of the panel. That is worth knowing in both directions: the cut is
rarer than it sounds, and the first person to meet it will be the first person
to see it.

That is a real cut and not a theoretical one -- the model is asked for one or
two sentences and will sometimes give five -- but it is also visible, which is
the part that matters while there is nothing to page with. Anything better
needs an interaction the device does not have yet: the three buttons do
nothing outside a question, and which of them would mean "more" is a UI/UX
question rather than a display one. A smaller face for a long answer is the
other candidate, and it trades the thing the panel is for -- being readable
from across the kitchen -- for text nobody asked to be complete.

Whatever it turns into, the layout already knows when it has run out of room:
`textDrawWrapped()` returns the lines it drew and `textWrapLines()` says how
many there were.

## D3 -- Battery accuracy and low-battery behaviour

The indicator now reads BQ27220 state of charge on Listening, answer and error;
Working and deep sleep retain it. Bus errors and out-of-range values show `?`.
The gauge configuration is left untouched. Its estimate still needs checking
against this battery over a discharge cycle in [E5](experiments.md); displaying
a percentage does not establish its accuracy. Acting on low charge remains open.

## D7 -- Nothing for a press too short

A press shorter than `config::kButtonMinHoldMs` sends no audio and has no
dedicated rejection screen or chirp. It fetches the idle dashboard before
sleeping, like other returns to idle.

"No sound" cannot be quite true, because the ready chirp comes first by design.
The chirp sounds 104 ms after the wake, with capture already running --
[E1](experiments.md), re-run on the firmware at
[S9](implementation.md#s9----re-run-e1) -- while the minimum hold is 300 ms. So a tap between the
two has already been answered with "the microphone is live", which was true when
it sounded. There is no further voice feedback; the dashboard fetch is silent.

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

## D10 -- Dashboard compression

The [dashboard plan](../../server/docs/use-cases/idle-screen.md) starts with
an uncompressed packed 800x480 one-bit frame: exactly 48000 bytes. This is an
agreed first implementation, not a claim that compression is unnecessary.
Choose the design first, then compare representative frames and device costs
in E10. RLE, gzip, PNG and Group 4 are device-transport candidates. The server can
already return PNG for browser preview; the firmware still consumes mono1-v1.

## D11 -- Energy during the post-answer wait

The dashboard flow waits ten seconds after the final voice screen
finishes drawing, with the CPU awake and WiFi retained. This is the initial
implementation choice; neither its energy nor that of sleeping and reconnecting
has been measured. E9 will compare both and measure a periodic dashboard cycle.
These measurements are explicitly deferred and do not block S15.
