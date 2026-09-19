#pragma once

// Template for src/secrets.h, which is gitignored and holds the real values.
// Copy this file to src/secrets.h and fill it in:
//
//     cp src/secrets.example.h src/secrets.h
//
// A missing src/secrets.h fails the build at the #include in main.cpp; empty
// values are caught at runtime and reported over Serial1.
//
// These end up in the firmware image as plain strings. Anyone who can read the
// flash can read them, so treat them as device-scoped credentials rather than
// anything reusable elsewhere.

namespace secrets {

// Network the device joins when it wakes up to record.
constexpr const char* kWifiSsid = "";
constexpr const char* kWifiPassword = "";

// Backend that takes the recording and answers with text, as http://host[:port]
// with no trailing slash; the path is config::kAudioPath. Here rather than in
// config.h because the repository is public and the backend's address need not
// be. Plain HTTP for now -- see docs/experiments.md, E2, before switching to
// HTTPS.
constexpr const char* kBackendBaseUrl = "";

// Sent to the backend with every question as `Authorization: Bearer`. The same
// string as DEVICE_TOKEN in the backend's environment; any long random one will
// do:
//
//     python3 -c 'import secrets; print(secrets.token_urlsafe(32))'
constexpr const char* kDeviceToken = "";

}  // namespace secrets
