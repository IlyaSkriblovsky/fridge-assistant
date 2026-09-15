#pragma once

#include <stdint.h>

// Non-secret project configuration. Credentials live in secrets.h, which is
// gitignored; anything here is safe to commit and is meant to be edited.

namespace config {

// Backend that takes the recording and answers with text. Plain HTTP for now --
// see docs/experiments.md, E2, before switching to HTTPS.
constexpr const char* kBackendBaseUrl = "http://192.168.10.178:8000";

// POST target for the recording: base URL + this path, Content-Type audio/wav.
constexpr const char* kAudioPath = "/audio";

// Recording stops here whether or not the button is still held. At 16 kHz mono
// 16-bit that is 32 KB/s, so 30 s is 960 KB of PSRAM.
constexpr uint32_t kMaxRecordSeconds = 30;

// How long to wait for the backend to answer after the upload finishes. The
// backend runs speech recognition and a language model, so this is seconds, not
// milliseconds. On expiry the connection is dropped and an error is shown.
constexpr uint32_t kResponseTimeoutMs = 30000;

// A press shorter than this is treated as an accidental tap and discarded
// without contacting the backend.
constexpr uint32_t kButtonMinHoldMs = 300;

// Release has to stay stable this long before recording stops; without it a
// contact bounce cuts the recording off mid-sentence.
constexpr uint32_t kButtonDebounceMs = 40;

}  // namespace config
