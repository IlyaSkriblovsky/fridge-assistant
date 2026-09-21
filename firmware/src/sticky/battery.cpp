#include "sticky/battery.h"

#include "sticky/sensor_bus.h"

int stickyBattery::readPercent() {
  if (!stickySensorBus::begin()) return -1;

  // TI BQ27220 TRM, StateOfCharge(): little-endian word at 0x2C.
  // https://www.ti.com/lit/ug/sluubd4a/sluubd4a.pdf (section 2.21)
  constexpr uint8_t address = 0x55;
  Wire.beginTransmission(address);
  Wire.write(0x2C);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(address, static_cast<uint8_t>(2)) != 2) return -1;
  const int low = Wire.read();
  const int high = Wire.read();
  if (low < 0 || high < 0) return -1;
  const int percent = low | (high << 8);
  return percent <= 100 ? percent : -1;
}
