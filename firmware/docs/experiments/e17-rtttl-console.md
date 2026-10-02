# E17 — RTTTL console on the device

[Experiment index](../experiments.md)

**Status and conclusion.** Built, flashed and USB command path checked on
2026-10-02. The user selected DeskPhon at octave 6 after device listening.
It accepts RTTTL from the computer over the bidirectional CH343P USB/UART bridge
and plays it through the production buzzer's PlayRtttl/LEDC path. No reflash is
needed between melodies. This is a subjective listening tool, not an SPL test.

## Question

Which RTTTL ringtone and octave sound pleasant and sufficiently audible on the
assembled device? E15's trills remained unpleasant after lowering their pitches;
the user's Ericcson RTTTL candidate sounds quiet at its original 493–880 Hz.
E12 found a louder region at 3.75–4 kHz, but does not settle ringtone preference.

## Method

From `firmware/`, with the board on USB:

```sh
~/.platformio/penv/bin/pio run -e exp_e17
~/.platformio/penv/bin/pio run -e exp_e17 -t upload --upload-port /dev/cu.usbmodem...
uv run tools/e17_rtttl_console.py --log docs/measurements/e17-session.txt
```

The Python tool detects a single CH343P automatically. With several devices,
supply the port as its positional argument. It installs pyserial via its PEP 723
metadata. Close any other serial monitor before upload or opening this console.
Port opening can reset the board; wait for `E17 READY`. The rig holds the power
latch and stays awake; display, microphone, Wi-Fi and server are unused.

Paste a complete RTTTL line and press Enter. Playback starts immediately and
replaces any current melody. `PLAY` reports the full string and octave shift;
`DONE` reports completion. The current production `config::kNotificationRtttl` string is initially loaded.

| Input | Action |
| --- | --- |
| RTTTL string | Validate, load and play at original octave |
| `:repeat` | Replay the loaded string at the current octave shift |
| `:stop` | Stop playback immediately and park GPIO48 low |
| `:up` / `:down` | Shift all notes by one octave and replay |
| `:reset` | Restore original octaves and replay |
| `:help` | Print device commands |
| `:quit` or Ctrl-C | Stop playback and close the computer console |

Octave shifts are cumulative, bounded to ±3, and rejected if any sounding note
would leave octaves 0–7. Explicit note octaves transpose along with default ones.
The RTTTL string is preserved; shift applies at the output. Duty remains the
production approximately 50%; octave commands are not volume controls.

Accepted input is standard RTTTL with printable ASCII names, `d`, `o`, `b`
headers (no duplicates), lowercase notes, sharps, dotted notes and rests, with
no spaces in headers/notes and no RTX loop/style extensions. Durations are
1/2/4/8/16/32, BPM 25–900, octaves 0–7. Limits: 2048 bytes, 512 notes/rests and
120 seconds per melody. Invalid or overlong lines do not interrupt the current
melody or replace the previous one. Overlong input is discarded through newline.
The player is polled rather than blocked, keeping stop/input responsive.

Keep the device's position, listening distance and room conditions fixed.
Compare original pitch with nearby octave shifts; take breaks. Save chosen
strings, shifts, date, setup and comments (pleasantness and audibility separately)
in `docs/measurements/`. The console log records transmitted strings and replies.
For adoption, encode the chosen octave changes in the production RTTTL string,
then flash the regular environment:

```sh
~/.platformio/penv/bin/pio run -e reterminal_e1005 -t upload --upload-port /dev/cu.usbmodem...
```

## Verification and limitations

Host tests exercise the real library/output path, validation, octave changes and
stopping during a note. Build and successful upload do not confirm physical
sound or USB receive behavior. Device acceptance: paste two melodies, interrupt
one with `:stop`, replay, compare octave shifts, reject malformed input, and
restore production firmware after choosing. Subjective loudness is not calibrated
sound pressure; no electrical drive changes or universally preferred pitch are
claimed. The user's choice is recorded below.

## Device command check, 2026-10-02

E17 was flashed onto the assembled ESP32-S3 unit. A computer session received
`E17 READY`, sent RTTTL and received `PLAY`, `DONE` and `STOP`. The check covered
malformed-input rejection, stop during a long note, +1 octave/reset, short-song
completion and repeat. The current DeskPhon production string was restored to
the loaded buffer and stopped before closing the port. The
[USB transcript](../measurements/e17-usb-smoke-2026-10-02.txt) records commands and
responses. Both E17 and production builds passed; all 43 host tests passed.
These results confirm the command path, not subjective sound quality.

## User listening decision, 2026-10-02

The user selected DeskPhon and saved it in `config::kNotificationRtttl` with
`d=8,o=6,b=500`, preserving the complete encoded repetitions and dotted rests.
Production playback therefore alternates approximately 1108/1397 Hz notes.
This is a personal preference based on device listening; exact distance and
room conditions were not recorded. The earlier command transcript used octave 5
and is not evidence of the chosen octave's sound quality. Three-minute reminder
repeat acceptance remains pending.
