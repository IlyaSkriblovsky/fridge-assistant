# E15 -- Melodies of full-length trills

[Experiment index](../experiments.md)

**Status and conclusion.** Measured 2026-09-25. `double-call` (0, +2, 0, +2)
won all five round-robin comparisons and the final 4:1 against `question`.
Both stages support it as the preferred candidate in this listener's run.
It uses four full 320 ms E14 trills with three 50 ms rests (1430 ms nominal).
The three-repeat version is available as `stickyBuzzer::notification()`;
reminder groups now use it, including in silent mode. Voice cues are unchanged.

## Candidates and timing

Offsets are semitones, applied to both base frequencies as
`round(base_hz * 2^(offset / 12))`. Zero is the original trill, not silence.
Every element retains eight alternating 40 ms notes (320 ms total), starting
on its lower frequency. All use 50% duty. Four elements with three 50 ms rests
give a nominal 1430 ms phrase; execution adds small timer/reconfiguration costs.

| UART name | Offsets | Intent |
| --- | --- | --- |
| `user-turn` | 0, -1, +2, +1 | User's dip, jump and settling finish |
| `staircase` | 0, +1, +2, +3 | Steady rise |
| `double-call` | 0, +2, 0, +2 | Repeated upward call |
| `arch` | 0, +2, +3, 0 | Rise and return |
| `question` | 0, -2, 0, +3 | Dip and high finish |
| `fanfare` | 0, 0, +2, +4 | Repeated base followed by an ascent |

These contours are design candidates, not measured optima. Transposition moves
some tones away from E12's loud region, so perceived loudness may change along
each phrase. The test selects an overall notification preference, not equal-SPL
melodies, musical pitch accuracy, or a probability of noticing an alert.

## Procedure

Commands run from `firmware/`:

```sh
~/.platformio/penv/bin/pio run -e exp_e15
~/.platformio/penv/bin/pio run -e exp_e15 -t upload --upload-port <port>
~/.platformio/penv/bin/pio device monitor --port <port> --baud 115200 --rts 0 --dtr 0 --filter log2file
```

Wait for the UART0 startup banner before beginning. Keep the device and listener
in the same positions. Do not read the UART choices during the test.

- AI starts the run or replays the current pair.
- Up (GPIO5) chooses the first complete melody, A.
- Down (GPIO6) chooses the second complete melody, B.

Listen to both entire phrases before voting. There is a 900 ms pause between
A and B, distinct from the 50 ms pauses inside a phrase. The next pair starts
after a vote. Buttons held through playback must be released before voting.

Choose the melody preferred for an energetic notification that should be
noticed without becoming unpleasant. All 15 unordered pairs appear once in a
shuffled order; A/B order is random. Each candidate has five opportunities to
win. Two score leaders then meet five times, alternating A/B position from a
random starting order (3/2 balance). Total: 20 comparisons plus any replays.
At completion the winner plays twice with a 900 ms pause.

Equal scores use seeded random priority. A tie crossing the finalist cutoff
is explicitly logged; a finalist selected this way is not a proven top-two
candidate. The UART reports the seed, every vote and replay, scores, and final.
Audio configuration/write errors invalidate the current trial and halt voting.

## Interpretation

Record all choices under `docs/measurements/` and summarize them here.
Consider both round-robin and final outcomes; a close final, a cutoff tie or a
ranking reversal warrants a focused repeat with the relevant candidates.
One listener's forced choices do not establish universal preference or measured
detectability. Listening through this sequence also does not establish how an
alert is noticed during an unrelated activity; that is a later practical check.

## Device session, 2026-09-25

The dedicated build and upload succeeded. One listener completed all 20 votes
with seed 1292644861 and 15 replay requests. No audio failure was logged.
The [complete UART transcript](../measurements/e15-notification-melodies-2026-09-25.txt)
preserves startup, replays and choices; line endings are normalized. Exact
listening distance and room conditions were not recorded.

| Candidate | Offsets | Round-robin wins |
| --- | --- | ---: |
| `double-call` | 0, +2, 0, +2 | 5/5 |
| `question` | 0, -2, 0, +3 | 4/5 |
| `user-turn` | 0, -1, +2, +1 | 2/5 |
| `staircase` | 0, +1, +2, +3 | 2/5 |
| `fanfare` | 0, 0, +2, +4 | 2/5 |
| `arch` | 0, +2, +3, 0 | 0/5 |

There was no tie at the finalist cutoff. The final choices were `question`,
then `double-call` four times, yielding 4:1. Including the first-stage meeting,
`double-call` won 5/6 direct comparisons against `question`, with wins in both
A/B positions. This is consistent within the run, not an independent replication.

The selected phrase alternates the base 3550/4150 Hz trill and its +2-semitone
transposition, rounded to 3985/4658 Hz: base, higher, base, higher.
Each element has eight 40 ms notes at 50% duty. This supports a preferred
notification melody among the six tested, not a measured guarantee that it will
be noticed during other activities.

After trying longer melodies on the computer, the user requested this motif
three times with pauses. It is now available as `stickyBuzzer::notification()`,
with 500 ms between the 1430 ms phrases (5290 ms nominal). That pause is a
design choice; the three-repeat arrangement has not been listening-tested on
the device. Reminder events now use this arrangement.

Verification on 2026-09-28: the production environment built using the empty
secrets template. A host harness executing the actual buzzer module verified
96 tones, nine 50 ms rests, two 500 ms rests, three identical phrases and
5290 ms total, plus silent-mode suppression, failed attachment and pin parking.
The assembled three-repeat signal has not been flashed or listening-tested.

## Production listening feedback, 2026-10-02

Real reminder use found the production pitches unpleasantly high. At the user's
request, production now trials 2500/2670 Hz and 2806/2997 Hz, retaining the
0/+2/0/+2 contour, timing and three phrases. The trill span is narrower so all
four tones fit 2.5–3 kHz. This is a new preference trial, not a revision of the
historical measurements above or a claim of equal loudness. Device listening
acceptance remains pending.

The user subsequently accepted the clock layout and reported working text
capitalization, but still found the lower trill unpleasant. Production now
uses the user-supplied Ericcson RTTTL ringtone through PlayRtttl instead of this
trill arrangement. Listening acceptance for Ericcson and battery repeat checks
remain pending; the experimental results above remain historical evidence.
