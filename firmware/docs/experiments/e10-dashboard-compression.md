# E10 -- Dashboard compression (deferred)

[Experiment index](../experiments.md)

**Status and conclusion.** PNG accepted on the device 2026-09-23. One smoke test downloaded 2323 bytes in 495 ms and decoded in 14 ms. Representative paired latency, memory and energy measurements remain deferred until layout selection.

Agreed 2026-09-21. On 2026-09-23 the user selected a firmware-only trial of the
server's existing 1bpp PNG. After choosing the layout, save representative
monochrome frames, including sparse and dense content.
Compare the 48000-byte raw baseline with candidate encodings, then measure
download plus decode time, peak memory, firmware size and energy on the device.
Verify exact pixel round trips. Host compression ratios alone do not show an
energy benefit. The user accepted PNG on 2026-09-23; mono1 and format selection are removed.

Firmware trial, 2026-09-23: one cold boot against the unchanged live server
downloaded a 2323-byte PNG (raw baseline 48000 bytes), with 495 ms HTTP request
time and 14 ms decode time on the ESP32-S3. The device completed a 2725 ms full
refresh and entered deep sleep. See the filtered
[UART log](../measurements/dashboard-png-2026-09-23.log). This is a functional
smoke test, not a paired latency or energy benchmark; sensor values and network
conditions can change between requests. Full E10 measurements remain deferred.
