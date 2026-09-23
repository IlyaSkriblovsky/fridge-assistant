# E2 -- HTTPS overhead

[Experiment index](../experiments.md)

**Status and conclusion.** Taken 2026-09-22 and repeated 2026-09-23. Verified HTTPS adds about 0.8 s to a fresh connection and 53 KiB internal RAM; good-signal streamed tail: 76 ms HTTP versus 80 ms HTTPS. The user chose to retain HTTP; migration is deferred under D5.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** The backend is meant to live outside the home network, which means TLS.
On a battery device that wakes for every question, the handshake is paid every
time. The original question was whether this made cloud hosting impractical.
The backend now lives in the cloud, and streaming starts during the hold: the
remaining decision is the cost of HTTPS for short questions and dashboard
fetches, and whether the streaming tail changes.

**What to measure.** Same request over `http://` and `https://`, comparing:

- Time from request start to the first response byte.
- Peak free heap during the request -- TLS buffers are tens of kilobytes and
  compete with the 960 KB audio buffer.

**How.** `src/experiments/e2_https_cost.cpp`, environment `exp_e2`, uses the
production `Backend`, `WifiLink` and `Recording` unchanged. A linker wrapper
around `esp_http_client_init` enables the framework's root certificate bundle
and records `HTTP_EVENT_ON_CONNECTED`. Verification of the hostname, chain and
validity dates stays enabled. SNTP establishes the clock on the first boot,
outside the request timer; RTC time survives the following sleeps.

The hostname comes from `secrets::kBackendBaseUrl` (a hostname without a port or
trailing slash). Both standard ports must serve `POST /audio/fault/500` directly,
with the same device token. No redirects are followed. Only an expected 500
with the complete upload counts as a valid measurement.

```
~/.platformio/penv/bin/pio run -e exp_e2 -t upload --upload-port <port>
~/.platformio/penv/bin/python tools/serial_log.py <port> /tmp/e2.log
python3 tools/e2_summary.py /tmp/e2.log
```

Stop the logger after `E2 DONE`, then restore the `reterminal_e1005` environment.
The rig sends one request per wake, sleeping for one second between requests:
12 adjacent HTTP/HTTPS pairs for each of two payload shapes, 48 requests total.
Order reverses on alternate blocks. All but the labelled initial power-on row
follow deep sleep; no connection or TLS ticket is retained. WiFi power saving
is the production default. The radio, DNS and Internet path are part of the
conditions, so use paired differences and medians, retaining outliers.

- **Tiny:** the 44-byte WAV header only, for the immediate-response comparison.
- **Stream:** 128044 bytes, four seconds of synthetic silence at 512 bytes per
  16 ms, starting after connection. This separates the connection cost from
  the steady-stream response tail. It does not simulate capture overlapping
  the handshake or prove end-to-end button-release latency.

The log's `E2 ROW` columns are cycle, scheme, shape, status, bytes,
DNS/TCP/TLS-to-connected time, open-through-request-headers time,
open-to-response-headers time, end-of-body-to-response-headers time,
synthetic-release-to-response-headers time, longest write, internal heap before,
and minimum internal heap during the request. Times are microseconds; heap is
bytes. The synthetic release is the final sample's nominal deadline, so its
tail includes any outstanding write. `Backend::firstByteUs()` actually marks
**complete response headers**, not the first wire byte; the 500 response body
is not read. There is no chirp or display delay before reading. Heap minima
use the allocator's local minimum monitor, with the production 960 KB recording
allocation already present in PSRAM. Display and capture tasks are not running,
so this measures request allocation cost rather than full-application headroom.

**Watch for.** Session resumption makes the second handshake much cheaper than
the first. Since every question starts from deep sleep, only the cold number
matters unless the session ticket is cached in RTC memory.

**Since [S11](e7-upload.md#firmware-streaming-validation) the handshake is not in
the wait on most questions.** The request opens while the button is held -- at
about 400 ms into the wake on a cached lease -- so the handshake is paid under
the hold, where the user is still talking, and what the scheme can still add to
the wait after the release is the encryption of the last few chunks and of the
answer. The handshake reaches the wait only when the release comes before the
request could open: a question shorter than the network, or one whose stale
lease is being renewed. So the comparison wants three numbers rather than one:
the handshake itself (`Backend::connectedUs()` against `openUs()`), the tail
after the release, and the heap. The first-byte time is the wrong instrument
for now -- the firmware reads the answer after the taken chirp, so a backend
that answers within 60 ms reads as 62 ms whatever the scheme; see
`Backend::firstByteUs()`.


## Result — 2026-09-23 (with the 2026-09-22 weak-signal comparison)

**Taken on the device against the production server**, using the user's supplied
HTTP and HTTPS fault URLs. Both returned 500 directly, without redirects. The
server hostname remains in the ignored secrets file, as required by the vision.
The certificate was verified using the framework CA bundle, not an insecure
TLS mode. Hardware: ESP32-S3 at 240 MHz, USB-powered, Arduino 3.3.7 / IDF library
5.5.0+87912cd291, TLS 1.2 enabled, production WiFi power-save settings. Firmware
base revision: `3444166`; measurement code is the E2 rig stored alongside this
record. One fresh connection per wake; WiFi association is excluded.

The first series completed even though the assistant's turn was interrupted.
The user then restored normal firmware and moved the device closer to the AP.
The second series was freshly flashed and captured on 2026-09-23. Each series
contains all 48 unique cycles, all with status 500 and the expected byte count.
No failed requests were removed. The initial power-on request is retained and
labelled; the remaining 47 requests follow deep sleep.

Raw logs: [2026-09-22](../measurements/e2-2026-09-22.log) and
[2026-09-23](../measurements/e2-2026-09-23.log). Run `tools/e2_summary.py` on either
file to reproduce the summary. RSSI was -88..-85 dBm (median -86.5) in the first
series, -67..-60 dBm (median -62) in the second. These are two sessions at
different times, not a controlled isolation of RSSI from Internet conditions.

**Good-signal series, 12 samples per cell.** Values below are median
[min..max], in milliseconds. "Response" means complete response headers; this
fault path does not consume the error body.

| Measurement | HTTP | Verified HTTPS |
| --- | --- | --- |
| Tiny request: DNS + TCP (+ TLS), to connected event | 156.9 [131.1..205.3] | 975.3 [920.6..1056.6] |
| Tiny request: open through sent request headers | 159.0 [133.1..207.3] | 977.9 [923.1..1059.2] |
| Tiny request: open to response | 265.2 [235.2..308.7] | 1061.6 [1038.4..1248.3] |
| Four-second stream: open to response, including four-second pacing | 4238.8 [4201.2..4343.5] | 5070.3 [5028.4..5134.6] |
| Stream: last body terminator sent to response | 72.7 [68.6..88.3] | 72.0 [70.0..76.2] |
| Stream: nominal last sample to response, including pending writes | 76.4 [71.8..91.5] | 79.9 [77.9..144.4] |

Adjacent-pair HTTPS-minus-HTTP medians are **800.4 ms** for the tiny request,
**810.6 ms** to the tiny request's connected event, and **3.8 ms** for the
stream's nominal-release tail. The latter's pairwise range is -5.6..72.5 ms.
The difference between medians is not in general the median of differences.
The connected event includes DNS and TCP as well as TLS: these measurements
locate the extra cost in connection setup but do not separate cryptographic
CPU work from TLS network round trips. With 12 samples, the nearest-rank p95
printed by the script is the maximum, not a well-estimated population tail.

**Memory.** Tiny HTTP requests reduced the local minimum of free internal RAM
by a median 4184 bytes; HTTPS by 58272 bytes: **54088 bytes / 52.8 KiB extra**.
The four-second stream uses 4752 vs 58272 bytes, about 52.3 KiB extra. The
lowest remaining internal heap in the good-signal HTTPS series was 206164
bytes. This is rig headroom, not proof of full-firmware headroom with capture,
display and dashboard tasks. The 960 KB PSRAM recording allocation was present.

**The weak-signal series was much less stable during upload.**

| Measurement, median (maximum), ms | Weak HTTP | Weak HTTPS | Good HTTP | Good HTTPS |
| --- | --- | --- | --- | --- |
| Tiny request, open to response | 301 (379) | 1227 (1671) | 265 (309) | 1062 (1248) |
| Stream, nominal last sample to response | 332 (10396) | 454 (3059) | 76 (92) | 80 (144) |
| Longest single write per stream | 76 (2715) | 86 (1406) | 27 (61) | 8 (71) |

Three of twelve weak-signal HTTP streams and one of twelve HTTPS streams had
a single write lasting at least one second. None did in the good-signal series.
That does not establish that TLS improves reliability: there are too few
samples, different packet shapes, and no packet capture. It does show why an
unpaired mean or one slow HTTP upload would misrepresent the TLS cost.

## What it changes

- **The connection overhead is noticeable: about 0.8 s, not a few milliseconds.**
  A short immediate-response request is about four times as long. Moving the
  device nearer the AP removed the observed multi-second upload stalls but
  did not remove the HTTPS connection cost.
- **Steady audio streaming has little additional tail latency in this run.**
  The production capture task continues during connection setup, so a long
  spoken question can overlap TLS with speech. This is an inference from the
  architecture, not a measured complete voice interaction: the rig starts its
  four-second stream after connection and does not exercise the initial audio
  backlog or short holds. Short questions can expose some of the 0.8 s.
- **Dashboard requests cannot hide setup under speech.** Expect an additional
  connection cost of this order for a fresh HTTPS fetch, but the actual 48000-byte
  dashboard download and rendering were not measured by this fault endpoint.
- **A migration looks practical, but “negligible overhead” would be wrong.**
  On 2026-09-23 the user chose to keep HTTP and defer migration under D5.
  E2 is complete; its measurements are retained. A future migration must enable trust and
  reliable time in both Backend and Dashboard, then verify short/long voice
  requests, the audio backlog, dashboard fetches and live memory headroom.
  Initial SNTP took 3891 ms in this run (3421 ms in the earlier run), outside
  request timing; all subsequent wakes retained time and needed no clock wait.
  A cold-boot clock policy therefore matters as well as the measured handshake.
- **This says nothing quantitative about battery life.** USB-powered timing
  and allocator minima are not integrated energy measurements; E9 remains due.

The normal firmware is restored after the experiment. No production transport,
credentials, WiFi power-save setting or server configuration is changed by E2.
