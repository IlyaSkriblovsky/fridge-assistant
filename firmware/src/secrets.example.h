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

// Bearer token for the backend. The backend's URL is not a secret and lives in
// config.h.
constexpr const char* kBackendToken = "";

}  // namespace secrets
