#pragma once

// Persistent preference. Load once before any buzzer or display work.
namespace silentMode {
bool load();
bool enabled();
bool toggle();  // false leaves the previous value in effect
}
