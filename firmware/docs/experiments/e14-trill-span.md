# E14 -- Trill span

[Experiment index](../experiments.md)

**Status and conclusion.** Measured 2026-09-25. The 600 Hz span at 3550/4150 Hz
and the 800 Hz span tied for the round-robin lead at 4/6, but the 600 Hz span
won both of their first-stage meetings and swept the five-vote final. Its 7/7
direct result supports 3550/4150 Hz as the notification trill. Production sound
has not yet been changed.

## Question

E13 selected an eight-note, 40 ms two-frequency warble, but its narrow and wide
versions split the round robin and final. What pitch interval makes that fixed
shape the preferred energetic notification?

## Method

The centre frequency is fixed at 3850 Hz, near the loud region measured by E12.
Every candidate uses eight contiguous 40 ms notes and 50% PWM duty. Only the
distance between alternating pitches changes:

| Candidate | Frequencies | Span |
| --- | --- | ---: |
| `span-400` | 3650 / 4050 Hz | 400 Hz |
| `span-600` | 3550 / 4150 Hz | 600 Hz |
| `span-800` | 3450 / 4250 Hz | 800 Hz |
| `span-1000` | 3350 / 4350 Hz | 1000 Hz |

Build and flash from `firmware/`:

```sh
~/.platformio/penv/bin/pio run -e exp_e14
~/.platformio/penv/bin/pio run -e exp_e14 -t upload --upload-port <port>
```

Do not watch UART while choosing. Press AI to start. Up chooses A, Down chooses
B, and AI replays the pair without voting. Choose the trill preferred as an
energetic and pleasant notification.

Every pair appears twice, with random trial and A/B order: 12 comparisons total.
Each candidate can score up to six wins. The two leaders then meet five times.
The odd final length prevents a tie. UART records the random seed, all revealed
choices, the repeated-round-robin scores and the final.

The repeated pairings can expose inconsistent choices, which are part of the
result rather than errors. A strong candidate should lead the first stage and
win the final; a reversal or narrow final calls for another run before changing
production. Save the transcript under `docs/measurements/` and summarize it here.

## Result, 2026-09-25

One listener completed the 12 repeated round-robin comparisons and five-vote
final. The full revealed sequence and seed are in
[the measurement record](../measurements/e14-trill-span-2026-09-25.txt).

| Candidate | Round-robin score |
| --- | ---: |
| `span-600`, 3550/4150 Hz | 4/6 |
| `span-800`, 3450/4250 Hz | 4/6 |
| `span-400`, 3650/4050 Hz | 3/6 |
| `span-1000`, 3350/4350 Hz | 1/6 |

The two leaders were not actually tied head to head: `span-600` won both of
their round-robin meetings. It then beat `span-800` 5:0 in the final, independent
of A/B position, for a combined 7/7 direct result. The inconsistent repeated
choices occurred against other candidates: 400 versus 600 Hz span and 600
versus 1000 Hz span each split 1:1.

E13 established the eight-note alternating shape; E14 supplies its contour:
3550, 4150, 3550, 4150, 3550, 4150, 3550, 4150 Hz, with every note lasting
40 ms, no gaps, and 50% PWM duty. This is now a measured candidate for production
notification patterns rather than an assumed one.
