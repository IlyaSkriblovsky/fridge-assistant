# E12 -- Buzzer loudness

[Experiment index](../experiments.md)

**Status and conclusion.** Measured 2026-09-25 in two blind runs. The frequency
winner repeated within one 250 Hz step: 3750 Hz, then 4000 Hz. Those optimized
signals beat the production 2500 Hz reference in all six final comparisons.
There is good subjective evidence that this assembled unit is louder around
3.75--4.0 kHz than at 2.5 kHz. Duty did not repeat: 50% won once and 37.5% once,
so the experiment does not support changing it yet. Production tones remain
unchanged pending a deliberate pattern change.

Commands and paths below are relative to `firmware/`.

## Question

What frequency and PWM duty make the reTerminal Sticky E1005's onboard buzzer
sound loudest in its assembled case? The production firmware currently uses
1500--3000 Hz and `ledcWriteTone()`, which always drives 50% duty. Those values
were chosen for distinguishable feedback, not measured for sound pressure.

Frequency matters because the transducer, its mounting and the case have a
frequency response. Duty changes the harmonic content of the square wave, so a
duty below 50% can sound louder if one of its harmonics lands on a strong
resonance. This rig does not change the GPIO voltage or the electrical drive
circuit, and therefore cannot answer whether a hardware driver could produce a
higher sound pressure.

## Method

Build and flash the dedicated environment:

```sh
~/.platformio/penv/bin/pio run -e exp_e12
~/.platformio/penv/bin/pio run -e exp_e12 -t upload --upload-port <port>
```

The rig holds the board's power latch, uses GPIO48 through LEDC at 10-bit
resolution, and leaves the display and microphone off. Connect the UART0 log at
115200 baud if a machine-readable record is wanted, but do not watch it while
choosing: it reveals the signals after every vote.

Put the assembled device in its normal position. Keep the listener's position,
device orientation and room conditions fixed. Press AI to begin. Every trial
plays A for 350 ms, waits 650 ms, then plays B for 350 ms:

| Button | Action |
| --- | --- |
| Up (GPIO5) | A was louder |
| AI (GPIO4) | Replay the same A/B pair; does not vote |
| Down (GPIO6) | B was louder |

The two candidates are randomly assigned to A and B on every trial. There are
three stages:

1. A single-elimination tournament compares 1000--6000 Hz in 250 Hz steps, all
   at 50% duty. Twenty comparisons select one of the 21 frequencies.
2. At that frequency, another tournament compares 12.5%, 25%, 37.5% and 50%
   duty. Three comparisons select one duty. Duties above 50% are omitted because
   a duty `d` and its complement `1-d` have the same AC spectral magnitudes for
   an ideal single-ended square-wave drive; they differ mainly in polarity and
   DC level.
3. The selected signal is compared three times with the production reference,
   2500 Hz at 50% duty. The order is independently randomised each time. This
   stage is skipped when the selected signal is already identical to the
   reference.

An ascending three-note sound marks completion. Save the UART transcript after
the run; it contains the random seed, every revealed pair and choice, both
tournament winners, and the final score against production.

## Acceptance and limitations

Run the experiment at least twice after a short break. Adopt a new production
setting only if the same neighbourhood wins again and the optimized signal wins
at least two of the three final comparisons in both runs. If the winner moves by
one 250 Hz step or choices repeatedly feel equal, treat that region as a plateau
rather than claiming an exact optimum.

This is a subjective audibility experiment, not an SPL measurement. A forced
choice makes close or equal signals look different, a knockout tournament can
eliminate a good candidate after one noisy judgment, and hearing sensitivity
varies strongly with frequency. Replays and repeated complete runs reduce those
effects but do not produce decibels. The 1--6 kHz search interval can also miss
a resonance outside it. A calibrated microphone at a fixed distance would be a
separate follow-up if an absolute level or small differences matter.

For later runs, record the date, physical setup, UART transcript and the
listener's comments. Keep the unmeasured electrical explanation above separate
from the observed result.

## Result, 2026-09-25

One listener completed two 26-choice runs with a short break between them. The
assembled device stayed in a fixed position during each run. Exact distance and
room conditions were not recorded. The full revealed choices and random seeds
are in [the measurement record](../measurements/e12-buzzer-loudness-2026-09-25.txt).

| Run | Frequency winner | Duty winner | Against 2500 Hz / 50% |
| --- | --- | --- | --- |
| 1 | 3750 Hz | 50% | 3/3 |
| 2 | 4000 Hz | 37.5% | 3/3 |

This satisfies the predeclared frequency criterion: the winners occupy the same
250 Hz neighbourhood and both optimized signals won at least two final trials.
It does not establish whether 3750 or 4000 Hz is the exact peak, nor whether
37.5% is better than 50%. A focused follow-up should compare several frequencies
around 3.75--4.0 kHz repeatedly and cross both leading duties at each frequency;
the knockout bracket used here confounds the frequency and duty conclusions.
