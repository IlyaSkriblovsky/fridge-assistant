#include "silent_mode.h"

#include <Preferences.h>

namespace {
bool muted = false;
}

bool silentMode::load() {
  Preferences prefs;
  // Read/write open creates the namespace on the first boot.
  if (!prefs.begin("sound", false)) {
    muted = true;  // an unreadable preference must not wake the household
    return false;
  }
  muted = prefs.getBool("silent", false);
  prefs.end();
  return true;
}

bool silentMode::enabled() { return muted; }

bool silentMode::toggle() {
  Preferences prefs;
  if (!prefs.begin("sound", false)) return false;
  const bool saved = prefs.putBool("silent", !muted) == 1;
  prefs.end();
  if (saved) muted = !muted;
  return saved;
}
