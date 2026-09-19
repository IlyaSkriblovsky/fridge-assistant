#pragma once

#include <stdint.h>

// Non-secret project configuration. Credentials live in secrets.h, which is
// gitignored; anything here is safe to commit and is meant to be edited.

namespace config {

// Backend that takes the recording and answers with text. Plain HTTP for now --
// see docs/experiments.md, E2, before switching to HTTPS.
constexpr const char* kBackendBaseUrl = "http://192.168.10.31:8000";

// POST target for the recording: base URL + this path, Content-Type audio/wav.
constexpr const char* kAudioPath = "/audio";

// How long a cached access point gets before the firmware stops believing it.
// The BSSID and channel of the last successful connect are kept in RTC memory
// across the sleep, which saves a scan on every question -- but the AP may have
// moved channel, or be a different radio of the same network, or be gone. Past
// this the attempt starts again as a plain scan, inside the budget below.
//
// It only has to be longer than a cached association that is going to work, and
// shorter than the patience for one that is not. Measured on the home network a
// cached link comes up in 77-158 ms, so this is twenty times over -- kept wide
// because nothing has yet measured an association that is slow and still
// succeeds, and shortening it would be a guess in the other direction. See S6
// in docs/implementation.md.
constexpr uint32_t kWifiCachedApAttemptMs = 3000;

// The whole association budget, the attempt on the cached AP included. Past
// this the question is over and the screen says NO WIFI.
//
// The 30 s recording cap is the ceiling this sits under: an association that
// has not happened by then has nowhere to go anyway. It is well short of it
// because the user is holding a button throughout, and a question that takes
// longer to send than to ask is a failure whatever the radio thinks.
//
// The budget is spent as one attempt on the cached AP and then fresh scans of
// 6 s each, which is 3 + 6 + 6 -- this number exactly, so changing either
// constant changes how many attempts fit.
constexpr uint32_t kWifiConnectTimeoutMs = 15000;

// Recording stops here whether or not the button is still held. At 16 kHz mono
// 16-bit that is 32 KB/s, so 30 s is 960 KB of PSRAM.
constexpr uint32_t kMaxRecordSeconds = 30;

// How long to wait for the backend to answer once the terminating chunk is
// written. The backend runs speech recognition and a language model, so this is
// seconds, not milliseconds. On expiry the connection is dropped and an error is
// shown.
//
// It is the backend's thinking time and nothing else: it counts from the
// terminating chunk, so a 30 s hold does not spend it.
constexpr uint32_t kResponseTimeoutMs = 30000;

// How long the TCP connection to the backend gets before the device decides
// there is nothing at that address. Separate from the wait above, which is for
// a backend that is thinking: one is normal and the other is not.
//
// It is also the price of being wrong about the network, which is what makes it
// a constant of ours rather than the HTTP client's own default. The failed
// connect is the only thing that can tell a stale cached DHCP lease from a good
// one, so this number is exactly what the wake that notices costs before it
// starts DHCP and asks again.
constexpr uint32_t kBackendConnectTimeoutMs = 5000;

// How long the open connection may go without taking a byte of the body before
// the device decides the backend has gone. It is a third wait with a third
// question: the connection answered, so the address works, and a backend that
// has not started thinking cannot be slow at it.
//
// It has to be longer than a stall a working connection recovers from. E7
// measured those: the device keeps 5744 bytes in flight, a lost ACK stops the
// window for a retransmission timeout, and the longest stall in 64 uploads was
// 4243 ms. The slowest first retransmit it saw was 2539 ms, and lwIP doubles
// the timeout for the next one, so a second loss straight behind that first
// would come to 7.6 s. This is past both. Under the hold a stall costs the user
// nothing, so the only price of a long wait is a dead backend noticed that much
// later.
constexpr uint32_t kBackendStallTimeoutMs = 10000;

// A press shorter than this is treated as an accidental tap and discarded
// without contacting the backend.
constexpr uint32_t kButtonMinHoldMs = 300;

// Release has to stay stable this long before recording stops; without it a
// contact bounce cuts the recording off mid-sentence.
constexpr uint32_t kButtonDebounceMs = 40;

}  // namespace config
