#pragma once

namespace stickyClimate {
struct Reading {
  float temperatureC = 0;
  float humidityPercent = 0;
  bool valid = false;
};
// SHT40; display task only, sharing the sensor bus with the battery gauge.
Reading read();
}
