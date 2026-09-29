# E16 -- Eight-element notification melodies

[Experiment index](../experiments.md)

**Status and conclusion.** The user disliked all computer-played candidates
and chose to return to E15's 0/+2/0/+2 motif, repeated three times with pauses.
No scored E16 transcript has been collected; this is qualitative user feedback.
The embedded build and Python host checks passed, but E16 was not flashed.

## Question and candidates

Does a longer, more developed melody improve the preferred notification?
E15 preferred `0, +2, 0, +2` among six four-element phrases. E16 keeps the
full 320 ms E14 trill in every element and extends phrases to eight elements.
Its reference repeats the winning motif twice, so all candidates share duration.
This does not directly test whether a longer alert is preferable to the original
1.43 s alert.

Offsets are semitones from the 3550/4150 Hz base trill. Both frequencies scale
by `2^(offset/12)`, rounded to integer Hz. Every element contains eight
alternating 40 ms notes, starting low, at 50% duty. Seven 50 ms pauses give
2910 ms nominal total duration (plus small execution overhead).

| UART name | Semitone offsets | Intended shape |
| --- | --- | --- |
| `repeated-call` | 0, 2, 0, 2, 0, 2, 0, 2 | E15 reference motif twice |
| `rising-answer` | 0, 2, 0, 2, 1, 3, 1, 3 | Second half answers a semitone higher |
| `wave` | 0, 2, 4, 2, 0, -2, 0, 2 | Crest, dip, recovery |
| `turn-and-answer` | 0, -1, 2, 1, 0, 2, 1, 3 | User's E15 turn with a new rising answer |
| `stepped-fanfare` | 0, 0, 2, 0, 2, 2, 4, 2 | Repeated pitches and a shifted second motif |
| `return-home` | 0, 2, 4, 2, 3, 1, 2, 0 | Two arches resolving to the base |

These are design candidates. Transposition can change loudness through the
device's frequency response; only duration, pauses, duty and element shape
are held constant, not sound pressure.

## Procedure

### Computer version

With the device busy, run the standard-library-only script from the repository:

```sh
python3 firmware/tools/e16_melodies.py
```

macOS uses its built-in `afplay`; Linux needs `aplay` or `ffplay`, and Windows
uses `winsound`. No Python packages are required. Press Enter to begin, then
type A or B after each complete pair; R or Enter repeats and Q exits. Ctrl+C
also stops with completed votes preserved. A stopped run does not resume.

The computer version uses the same six contours, semitone rounding, 320 ms
elements, 50 ms internal pauses, 900 ms A/B gap and 15+5 comparison protocol.
Each pair is a single mono 48 kHz, 16-bit WAV to keep the A/B gap exact.
It synthesizes a band-limited 50% square wave from odd harmonics below Nyquist,
with fixed headroom and no per-melody normalization. Digital volume defaults to
0.2; `--volume` accepts values greater than zero and at most one. Keep system
volume and playback hardware fixed across choices.

This approximates the electrical waveform, not the buzzer, enclosure or
frequency response. Speaker/headphone preferences are recorded as
`E16-computer` and must be distinguished from device results. Validate a
preferred melody on the device later.

Scores and identities stay hidden until completion. The JSON log is saved in
the current directory after every presentation and vote, including seed,
patterns, synthesis parameters, replays, cutoff ties and errors.
`--output path.json` chooses a new result path; an existing file is not
overwritten. `--seed N` reproduces the Python randomization for the same choices
(it does not reproduce the ESP32's xorshift ordering).

To export all six named WAVs without playing or running a test:

```sh
python3 firmware/tools/e16_melodies.py --export-wav /tmp/e16-melodies
```

Host verification checked six WAVs of exactly 139680 frames (2.91 s), pause
samples and PCM bounds; a simulated 20-vote session checked pair coverage,
replay, invalid input, final A/B balance and persisted JSON. Simulated playback
failure did not produce a vote. Actual speaker output and preference are not
verified by these checks.

### Device version

From `firmware/`:

```sh
~/.platformio/penv/bin/pio run -e exp_e16
~/.platformio/penv/bin/pio run -e exp_e16 -t upload --upload-port <port>
~/.platformio/penv/bin/pio device monitor --port <port> --baud 115200 --rts 0 --dtr 0 --filter log2file
```

Start with AI. Listen to the entire first melody (A), a 900 ms gap, and the
entire second melody (B), then vote. Up chooses A, Down chooses B; AI replays
the same pair. Keep listening position fixed and do not read UART names while
choosing. Choose the preferred energetic, noticeable, pleasant notification.
Each pair takes about 6.72 s to play. Pause as needed before voting.

The E15 protocol is retained: all 15 unordered pairs once in shuffled order
with random A/B positions, then five comparisons of the two score leaders.
Final positions alternate from a random starting order (3/2 balance). A seeded
random priority breaks equal scores; a tie at the finalist cutoff is logged.
Replays do not vote. Audio failures halt voting. Completion plays the winner
twice. Total: 20 votes plus any replays, typically several minutes.

Save the complete UART transcript in `docs/measurements/`. Report first-stage
scores and final separately, including cutoff ties, reversals and replays.
A single listener's run selects among these candidates; it does not establish
universal preference, detectability during other activities, or a benefit over
the shorter E15 winner. The user subsequently rejected these candidates
qualitatively; no numerical ranking is claimed.

## Build and device check, 2026-09-25

`pio run -e exp_e16` succeeded. The upload attempt reported an exclusive-lock
failure on `/dev/cu.usbmodem5C843360331`; the device node was absent on the next
check. E16 has not been verified as flashed or running. Reconnect the device,
resolve its port again and attach UART capture before the listening run.
