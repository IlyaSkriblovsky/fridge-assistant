# E11 -- RTC PNG restoration before LISTENING (rejected)

[Experiment index](../experiments.md)

**Status and conclusion.** Rejected 2026-09-23. Restoring the previous image cost 903–920 ms and LISTENING took 2445–2477 ms overall. The user found it slower; full LISTENING refresh was restored.

**Question.** Since the idle dashboard already receives a full refresh, can the
next LISTENING use partial refresh without writing the previous image to flash?

**Trial.** Retain the exact server PNG in one 7168-byte RTC FAST buffer, committing
it only after a successful dashboard refresh. On deep-sleep wake, validate the
cache, decode it into a temporary 48000-byte bitmap, draw that bitmap and the
remembered silent/stale indicators into the panel framebuffer, prime the driver's
previous-image shadow without an optical update, then draw LISTENING and perform
a whole-screen partial refresh. Oversized/invalid caches fall back to full.
No cache writes went to flash. Diagnostics ran on the display task and were
printed by the orchestrator after the voice result, when the panel was idle.

**Evidence.** Four deep-sleep voice wakes on the device, supplied by the user
on 2026-09-23. The retained PNG was 1852–1872 bytes, comfortably within the buffer;
all four restorations succeeded. Exact relevant UART lines are retained in the
[filtered log](../measurements/rtc-png-listening-2026-09-23.log).

| Phase | Run 1 | Run 2 | Run 3 | Run 4 |
| --- | ---: | ---: | ---: | ---: |
| PNG bytes | 1859 | 1852 | 1852 | 1872 |
| Validate RTC PNG, ms | 1.714 | 1.615 | 1.648 | 1.576 |
| Allocate bitmap, ms | 0.025 | 0.025 | 0.025 | 0.029 |
| Copy + decode PNG, ms | 16.581 | 16.515 | 16.608 | 16.849 |
| Redraw bitmap + overlays, ms | 751.947 | 745.031 | 760.202 | 748.756 |
| Prime previous-image shadow, ms | 136.967 | 139.921 | 141.745 | 136.022 |
| **Restore total, ms** | **907.274** | **903.147** | **920.267** | **903.271** |
| Prepare LISTENING excluding restore, ms | 321.877 | 325.270 | 322.938 | 323.454 |
| Panel update, ms | 1237.609 | 1216.795 | 1234.126 | 1230.609 |
| **LISTENING total, ms (integer log)** | **2466** | **2445** | **2477** | **2457** |

These are wall-clock durations, including scheduling. Copy/decode includes its
scratch allocations; restoration total includes cleanup and bookkeeping, so it
slightly exceeds the sum of its listed stages. Panel update includes transfers,
waveform and controller sleep, not just the optical waveform. LISTENING total
includes restoration and drawing; it is not a pure partial-waveform measurement.

**Finding.** PNG decoding is not the bottleneck: it takes about 17 ms. Rebuilding
the previous framebuffer through drawBitmap and overlays costs about 0.75 s;
with shadow priming, restoration adds about 0.91 s before the new screen. This
consumes the practical benefit of the shorter partial update. The resulting
2.45–2.48 s is comparable to the historical full-LISTENING cycle (about 2.4 s).
The user reports a clean image but worse perceived responsiveness: the full
refresh exposes readable text partway through its cycle, while this trial spends
substantial time reconstructing the old frame before driving the new image.

**Limits.** This log does not contain a paired full-LISTENING control or an
optical time-to-readable measurement; the perceptual comparison is user feedback.
All four requests subsequently timed out after 30 s, but LISTENING finished
roughly 2.85 s after setup began, before button release or backend timeout. The
log does not establish a causal link between the display change and those
network failures. No energy measurements were taken.

**Decision.** Reject this implementation and restore full refresh for LISTENING.
Remove the retained-PNG cache and its diagnostic code/tests. Keep the server PNG
transport, ordinary partial updates after LISTENING, and full dashboard refresh.
Retain these measurements so a future proposal must account for framebuffer
reconstruction and time to readable text, not only PNG decode or waveform time.

## Rollback verification

On 2026-09-23 the restored production firmware built, the original 28 native
tests passed, and the firmware was uploaded with flash-hash verification and
reset. Production code/tests matched the pre-trial revision; only experiment
documentation and filtered evidence remained. This confirms rollback/build/flash,
not a new visual acceptance of every dashboard path.
