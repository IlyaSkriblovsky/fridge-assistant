# E13 -- Notification trills

[Experiment index](../experiments.md)

**Status and conclusion.** Measured 2026-09-25. The two fast alternating
two-note patterns clearly led the other four shapes. `close-warble` won all five
round-robin comparisons and `wide-warble` won four, losing only their direct
comparison. The three-vote rematch reversed that pair: `wide-warble` won 2:1.
The experiment selects a fast two-note warble, but does not distinguish its
pitch interval reliably enough to choose the production contour yet.

Commands and paths below are relative to `firmware/`.

## Question

Which short contour sounds energetic and pleasant enough to use for a device
notification? E12 found that this assembled unit sounds louder around
3.75--4.0 kHz than at 2.5 kHz, but a sustained note at the louder frequency is
not a suitable notification by itself.

## Candidates

All six candidates last 320 ms, use 50% PWM duty, and spend most of their time
around the E12 peak. They vary only in pitch contour and rhythm:

| Name | Shape |
| --- | --- |
| `close-warble` | 3650 and 4050 Hz alternating every 40 ms |
| `wide-warble` | 3300 and 4300 Hz alternating every 40 ms |
| `double-rise` | two three-note rises separated by 20 ms |
| `rhythmic` | articulated high pulses followed by a rising finish |
| `spark` | irregular wide upward jumps |
| `arc` | an eight-step rise and fall |

Equal duration prevents a longer candidate from winning merely by supplying
more sound. This is a preference test, so silence and articulation inside that
duration are part of the candidate rather than normalized away.

## Method

Build and flash the dedicated environment:

```sh
~/.platformio/penv/bin/pio run -e exp_e13
~/.platformio/penv/bin/pio run -e exp_e13 -t upload --upload-port <port>
```

Keep the device and listener fixed. Do not watch the UART log while choosing;
it reveals candidate names after each vote. Press AI to begin. Each trial plays
A, waits 700 ms, then plays B:

| Button | Action |
| --- | --- |
| Up (GPIO5) | Prefer A as an energetic notification |
| AI (GPIO4) | Replay the same pair without voting |
| Down (GPIO6) | Prefer B as an energetic notification |

The rig presents every pair once in random order, for 15 comparisons. Each win
adds one point. It then presents the two highest-scoring candidates three more
times with independently random A/B order. The best-of-three winner plays twice
to mark completion. UART records the seed, revealed choices, scores and final.

One round robin is exploratory evidence rather than a stable preference model.
Repeat the run after a break if the finalists are close, if choices feel forced,
or before changing production behavior. Save the full transcript under
`docs/measurements/` and summarize the result here.

## Result, 2026-09-25

One listener completed all 15 round-robin comparisons and the three-vote final.
The full revealed sequence and seed are in
[the measurement record](../measurements/e13-notification-trills-2026-09-25.txt).

| Candidate | Round-robin score |
| --- | ---: |
| `close-warble` | 5/5 |
| `wide-warble` | 4/5 |
| `spark` | 3/5 |
| `double-rise` | 2/5 |
| `arc` | 1/5 |
| `rhythmic` | 0/5 |

The final was `wide-warble` 2, `close-warble` 1. This reversal is evidence that
their preference difference is small or context-dependent, rather than evidence
that the final necessarily overrides the complete round robin. Both share the
same essential shape: eight contiguous 40 ms notes alternating between a lower
and a higher pitch. They differ only in span: 3650--4050 Hz versus 3300--4300 Hz.

A focused follow-up should hold timing at eight 40 ms notes and compare several
spans around the 3.75--4.0 kHz loud region repeatedly. Production sounds remain
unchanged until that comparison establishes a stable contour.
