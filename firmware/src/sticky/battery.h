#pragma once

namespace stickyBattery {
// BQ27220 state of charge, 0..100; -1 when unavailable. Call only after boot,
// from the display task: the sensor bus uses the GPIO0 strapping pin.
int readPercent();
}
