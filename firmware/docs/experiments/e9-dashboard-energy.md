# E9 -- Dashboard energy (deferred)

[Experiment index](../experiments.md)

**Status and conclusion.** Deferred; not taken. Compare complete dashboard-cycle energy and awake post-answer wait against sleeping and reconnecting. No energy advantage is established.

Agreed 2026-09-21; not taken and not required before S15. Measure energy from
battery over a full periodic wake: boot, sensors, association, download, panel
refresh and return to sleep. Include failed-network attempts as a separate case.
Compare the ten-second post-answer wait with WiFi retained against sleeping
for that interval and reconnecting, with otherwise identical dashboard work.
Use the actual transport; if HTTPS is introduced, include its handshake.
Record integrated energy, not just latency or a single current reading, before
changing D11. E5 still owns the board's baseline deep-sleep current.
