# Current implementation

This is the firmware architecture and verification state of the checked-in
code. Update the relevant section when behavior changes; do not append a change
journal. Earlier build stages and superseded implementations are in Git history.

Read [project-vision.md](project-vision.md) for product decisions and hardware
constraints, [experiments.md](experiments.md) to find measurement evidence, and
[deferred.md](deferred.md) for deliberate limitations. The module map and build
commands are in [AGENTS.md](../AGENTS.md). Read detailed experiment records only
when the task needs them.

## Voice cycle

[main.cpp](../src/main.cpp) owns the flow; `loop()` alternates idle work and
voice cycles without recursion.

1. `setup()` holds the power latch immediately, enables AI/Up wake, initializes
   UART0 logging through `Serial1`, and loads the sound preference before any
   chirp. Up wake takes priority, including simultaneous AI/Up wake.
2. If AI is held, initialize the microphone, allocate/reuse the recording, start
   capture, then sound ready. Capture must already be running when ready sounds;
   nothing that can block belongs between starting capture and that chirp.
3. Start WiFi if needed and post LISTENING. Cold startup schedules an
   unsupersedable pre-clear. Drawing and recording proceed independently.
4. Open the audio request only when WiFi is online and
   `Capture::pastMinimumHold()` is true. Stream committed samples while held.
   A wall-clock threshold alone can misclassify a release still in debounce.
5. Capture stops on stable release, the 30-second cap, abort or read failure.
   A discarded tap uploads nothing and goes straight to dashboard fetch.
6. For a valid question, post WORKING. If streaming already started, send its
   remaining bytes and terminating chunk before the taken chirp, letting the
   server work during that chirp. Otherwise wait for WiFi within its budget,
   then send the recording. Read the JSON answer and post answer or error;
   sound the corresponding chirp immediately after posting.
7. Stop capture, close audio transport and enter the idle controller. A held
   button after a cap/failure must be released before a fresh press can start
   another question. Wait for the final display refresh before starting dwell.

WiFi or upload failure during recording aborts the question. Error classification
belongs to `Backend` and the orchestrator; the product error table is in the
vision. Large diagnostic logs are emitted after the last chirp, because UART
printing itself can add noticeable latency.

## Tasks and data ownership

| Owner | Responsibility | Boundary |
| --- | --- | --- |
| Orchestrator, Arduino loop on core 1 | Wake routing, WiFi polling, audio HTTP, chirps, idle state | Posts display work; never performs capture reads |
| Capture, core 1 / priority 10 | I2S reads and AI release polling | Owns microphone and button while running; sole recording writer |
| Display, core 0 / priority 1 | StickyScreen, panel, sensor reads | Sole screen and sensor I2C owner; publishes sensor snapshots |
| Dashboard worker | HTTP download, validation, PNG decode | Receives sensor values; never touches panel or I2C |

[Recording](../src/recording.h) owns a linear PSRAM allocation: a 44-byte WAV
header followed by at most 960000 bytes of 16 kHz, mono, signed 16-bit PCM.
Capture writes directly into it, then publishes the committed count with
release/acquire ordering. The uploader reads only published bytes; committed
samples never move. Both streaming WAV length fields are `0xFFFFFFFF`.

The microphone DMA holds only 90 ms. Capture reads 256 samples (16 ms) at a
time and polls release between reads; no network, drawing or logging belongs
in that task. Stop/join capture before reusing its microphone or button state.
Compare captured duration with the task's elapsed time to detect lost audio;
a long blocking read by itself does not measure time spent away from DMA.

Display has one pending slot: a newer post replaces work not yet started.
An active refresh always finishes, and the cold-start pre-clear cannot be
replaced. Thus an answer can supersede WORKING while LISTENING is still drawing.
Text is copied into the slot. Dashboard pixels are borrowed and must remain
alive and unchanged until `Display::waitIdle()` succeeds. Display records are
read after it becomes idle, when its worker is no longer changing them.

Dashboard cancellation requests socket shutdown without joining the worker on
the capture path. A connect in progress exits on its timeout. Keep the client
and buffers alive until completion; do not start a replacement download while
Display still borrows the previous bitmap.

## Network and streaming

[WifiLink](../src/wifi_link.h) caches BSSID/channel and the DHCP lease in RTC
memory. A cached-AP attempt gets 3 seconds, then scanning falls back within the
15-second overall budget. Reuse an address only within half its lease lifetime
and on the matching network. Set `WiFi.persistent(false)` before `WiFi.mode()`
to avoid flash-backed startup overhead; see [E6](experiments/e6-dhcp.md).

For voice, two unreachable connects on a reused lease trigger DHCP renewal and
another open. A refused connection or HTTP error proves reachability and does
not invalidate the lease. Each new open restarts from the WAV header; do not
retry a partially sent request as if nothing had been sent.

An optional IPv4 `secrets::kDnsServer` replaces the network resolver after the
address is acquired, including renewal. Cache the network's DNS first so
removing the override takes effect on the next wake. Installing it before DHCP
would allow the ACK to overwrite it.

[Backend](../src/backend.h) uses `esp_http_client` with explicit HTTP chunk
framing and Bearer authentication. The response must be valid JSON containing
string `response`. Connect, stalled-body and response budgets are separate;
the 60-second response budget starts at the terminating chunk. Values and routes belong
in [config.h](../src/config.h), credentials in gitignored `secrets.h` with a
tracked [example](../src/secrets.example.h).

Production remains HTTP by the user's decision after
[E2](experiments/e2-https-overhead.md); HTTPS trust/clock support in the rig is
not a completed production migration. That work remains [D5](deferred.md#d5----https).
Streaming evidence in [E7](experiments/e7-upload.md) uses a fixed-response
prototype; it does not predict the current model's answer time.

## Idle dashboard

Cold/timer wakes without AI held, an AI wake already released at setup, and
discarded taps fetch the dashboard without audio upload. Completed voice
answer/error cycles retain WiFi and wait 10 seconds from the final refresh's
completion. AI remains responsive during dwell, sensor/display waits and fetch;
capture starts without waiting for a cancelled dashboard worker to join.

Display takes a battery/climate snapshot for the worker. Battery and climate
are rendered by the server; local voice screens retain only the silent icon.
The worker requests the plain `/sticky/dashboard` endpoint with available
sensor values. There is no local notebook screen or wire-format selector.
The shared [contract](../../server/docs/device-contract.md#дашборд) and
[scenario](../../server/docs/use-cases/idle-screen.md) own the server behavior.

[Dashboard](../src/dashboard.h) validates status, metadata, identity encoding,
complete Content-Length (1–65536 bytes), and the entire PNG before posting it.
The accepted subset is 800×480, 1-bit grayscale, no interlace/transparency,
IHDR/IDAT/IEND only. CRC, zlib checksum, chunk ordering, filters and exact output
size are checked. Split IDAT and all five PNG row filters are supported.
The bounded decoder uses PSRAM and the vendored miniz inflater; white PNG bits
become the display's MSB-first, 1=black, 48000-byte bitmap. Download and decode
share the 15-second request deadline and check cancellation.

A successful dashboard always uses a full refresh. `Next-Update-After` is
counted from complete valid body receipt, so decode, drawing, awake work and
intervening Up wakes spend the interval. Store the absolute deadline as raw RTC
slow-clock ticks; recalibration on boot must not rescale it. Valid intervals
clamp to 60–86400 seconds; missing/invalid values and failures use one hour.
An overdue deadline arms at least 60 seconds rather than a tight wake loop.

Failure preserves the previous screen, marking a known dashboard stale;
a cold startup may show NO DASHBOARD. Background attempts never chirp.
Before sleeping, finish display work and the worker, release wake buttons,
close networking and retain the latch through deep sleep. AI and Up wake stay
enabled alongside the timer. Energy comparisons remain [E9](experiments/e9-dashboard-energy.md)
and [E10](experiments/e10-dashboard-compression.md).

## Display invariants

[sticky/epaper.h](../src/sticky/epaper.h) corrects SSD1677 polarity and the
previous-image plane needed by partial refresh. Its sleep override removes a
measured redundant delay. Keep all three; see [E8](experiments/e8-refresh.md).
LISTENING begins with a full refresh after deep sleep; WORKING changes the word
band, and answer/error use whole-panel partial updates when the shadow is valid.
The RTC PNG restoration trial was rejected; no retained dashboard image is
used to reconstruct LISTENING ([E11](experiments/e11-rtc-png-restoration.md)).

A controller RAM window limits writes, not optical refresh. After panel power
loss, seed both **full** controller planes identically before applying an
indicator's old/new window transition. Otherwise random RAM outside the icon
corrupts the retained screen. This is RAM initialization without optical
activation; never replace it with initialization of only the window.
The [host regression](../test/test_epaper/) checks both planes and subsequent
partials, while physical image quality still needs a device check.

## Silent mode and indicator state

[silent_mode](../src/silent_mode.cpp) stores the preference in NVS, writing only
on toggle. Default is sound enabled; an unreadable preference mutes. Load it
before buzzer/display work. Up wake toggles offline, waits for display and stable
release, then sleeps while preserving the dashboard deadline. Up also toggles
during awake idle work, but is ignored during a voice question.

RTC state remembers successfully displayed silent/stale indicators and whether
their old pixels are known. Invalidate that knowledge before an update and
commit it after success. Reconstruct only reserved white indicator regions;
unknown previous state preserves the glass until a normal screen reconciles it.
Do not infer that the NVS preference is necessarily the icon currently visible.

## Text and fonts

[text.cpp](../src/text.cpp) owns UTF-8 decoding, measuring and drawing through
Latin/Greek/Cyrillic font ranges, punctuation and fallback. Never use Seeed_GFX2
`drawString()` or `textWidth()` for these strings: their GFXFF path walks bytes.
Each font range must stay below 64 KiB of bitmaps because the library reads
bitmap offsets as 16-bit. Regenerate all faces together with
[gfxfont.py](../tools/gfxfont.py) to avoid mixing rasterizers.

Answers wrap at spaces/newlines or within an overwide word, center as a block
inside the answer box, and reserve ellipsis width on the final visible line.
Measuring and drawing share advances. Copy text on UTF-8 boundaries; backend,
answer and error-detail buffers have distinct bounds. Long answers still have
no paging ([D2](deferred.md)). Use the [host preview](../tools/preview/README.md)
for layout; it cannot prove contrast or partial-refresh quality.

## Verification state

These are retained observations, not new device checks made by editing this
file. Build/host success does not establish hardware acceptance.

- Voice capture, streaming, error routing, display-task transitions and wrapped
  Cyrillic answers were checked on the device. Evidence is in
  [E3](experiments/e3-microphone-settle.md), [E7](experiments/e7-upload.md) and
  [E8](experiments/e8-refresh.md). Ellipsis overflow was checked only on the host.
- Silent toggling, buzzer suppression and preservation of surrounding pixels
  were accepted on 2026-09-21. Power-loss preference persistence and interrupted
  refresh recovery were not separately reported as tested.
- The initial dashboard worked on the device; PNG was visually accepted on
  2026-09-23. The [PNG smoke log](measurements/dashboard-png-2026-09-23.log)
  proves download/decode/refresh/sleep, not energy or interruption latency.
- Latest tap routing, server-rendered sensor placement, battery timer cycles,
  AI interruption during fetch/dwell, failure recovery and deadline preservation
  across Up wakes need explicit coverage; earlier acceptance does not prove
  each refinement. BQ27220/SHT40 readings worked before rendering moved server-side;
  gauge accuracy and climate comparison against a reference remain open.
- DNS override hardware validation and the E1 timer-wrapper rerun remain
  unrecorded. The restored production firmware was flashed after E11 rollback
  on 2026-09-23; flashing alone does not close the checks above.
- Firmware expects a PNG-serving backend. Verify that server rollout before
  installing on a deployment still returning raw mono1; no live deployment
  state is inferred here from checked-in code or a historical flash report.

[Host tests](../test/README) cover text, recording, controller RAM, dashboard
protocol/scheduling and PNG decoding. CI runs the native suite and builds every
embedded environment with example credentials. These checks do not validate
physical peripherals, task timing, concurrent visibility or network behavior.
