# Experiments

Read this index first, then only the experiment relevant to the task. Results
are evidence from the stated firmware, network and hardware conditions, not
universal timing guarantees. Current architecture is in
[implementation.md](implementation.md); open compromises are in
[deferred.md](deferred.md).

| Experiment | Question | Status | Conclusion |
| --- | --- | --- | --- |
| [E1](experiments/e1-wake-latency.md) | Wake to usable audio | Measured; firmware rerun 2026-09-18 | 104 ms to ready chirp; deep sleep retained |
| [E2](experiments/e2-https-overhead.md) | Cost of HTTPS | Measured 2026-09-23; migration deferred | ~0.8 s cold-connect overhead, ~53 KiB RAM; HTTP retained |
| [E3](experiments/e3-microphone-settle.md) | Microphone settle window | Measured | 24 ms discard configured |
| [E4](experiments/e4-button-long-press.md) | Hardware action on long AI hold | Measured | No hardware action through 20.4 s |
| [E5](experiments/e5-sleep-current.md) | Deep-sleep current | Not measured | Battery measurements still owed |
| [E6](experiments/e6-dhcp.md) | DHCP cost and lease caching | Measured | Cached lease removes the DHCP exchange |
| [E7](experiments/e7-upload.md) | Upload latency and stalls | Measured; streaming adopted | Streaming hides upload under the hold |
| [E8](experiments/e8-refresh.md) | Refresh phases and display queue | Measured; delay removed | 100 ms removed per refresh; panel queue remains |
| [E9](experiments/e9-dashboard-energy.md) | Dashboard and post-answer energy | Deferred | Compare awake wait with sleep/reconnect |
| [E10](experiments/e10-dashboard-compression.md) | Dashboard compression costs | PNG accepted; benchmark deferred | Functional smoke test only; no energy claim |
| [E11](experiments/e11-rtc-png-restoration.md) | Restore PNG before LISTENING | Rejected 2026-09-23 | Restoration cost outweighed perceived benefit |
| [E12](experiments/e12-buzzer-loudness.md) | Loudest useful buzzer drive | Measured 2026-09-25 | 3.75--4.0 kHz beat 2.5 kHz 6/6; best duty remains unresolved |
| [E13](experiments/e13-notification-trills.md) | Preferred energetic notification trill | Measured 2026-09-25 | Fast two-note warbles led; close won 5/5 round robin, wide won final 2:1 |
| [E14](experiments/e14-trill-span.md) | Preferred span of the two-note trill | Measured 2026-09-25 | 3550/4150 Hz beat the nearest finalist in all 7 direct comparisons |
| [E15](experiments/e15-notification-melodies.md) | Preferred melody of full-length trills | Measured 2026-09-25 | 0/+2/0/+2 won 5/5 round robin and 4:1 final; 1.43 s phrase |
| [E16](experiments/e16-long-melodies.md) | Preferred eight-element notification melody | Rejected by user after computer listening | Returned to E15 motif, requesting three repetitions with pauses |

| [E17](experiments/e17-rtttl-console.md) | RTTTL ringtone and octave listening on the device | Built/flashed; user selection 2026-10-02 | DeskPhon at octave 6 selected through live USB listening |

## Recording an experiment

Keep one file per experiment: conclusion and status first, then the question,
conditions/date, method, results, limitations and the decision they support.
Retain failed and rejected experiments when they explain a constraint. Update
the index when the status or conclusion changes; do not append run logs here.

Rigs live in `src/experiments/` with separate PlatformIO environments. E1 also
has a rig that wraps the production firmware at link time. Build/flash commands
inside experiment files run from `firmware/`. Reusable host tools live in
`tools/`; filtered evidence lives in [measurements/](measurements/).
Keep enough provenance and commands to reproduce a result. Distinguish a host
check, successful flash, serial observation and visual device acceptance.
