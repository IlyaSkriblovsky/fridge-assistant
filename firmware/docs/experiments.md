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
