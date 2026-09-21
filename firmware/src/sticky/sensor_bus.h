#pragma once

#include <Wire.h>

namespace stickySensorBus {
// Display task only, after boot: SCL is the GPIO0 strapping pin.
inline bool begin() {
  static bool ready = false;
  if (!ready) {
    Wire.setTimeOut(20);
    ready = Wire.begin(1, 0, 100000);
  }
  return ready;
}
}
