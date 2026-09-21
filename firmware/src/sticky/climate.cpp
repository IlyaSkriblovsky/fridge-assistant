#include "sticky/climate.h"
#include "sticky/sensor_bus.h"

namespace {
bool validCrc(const uint8_t* word) {
  uint8_t crc = 0xFF;
  for (int i = 0; i < 2; ++i) {
    crc ^= word[i];
    for (int bit = 0; bit < 8; ++bit)
      crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
  }
  return crc == word[2];
}
}

stickyClimate::Reading stickyClimate::read() {
  Reading result;
  if (!stickySensorBus::begin()) return result;
  // Sensirion SHT4x datasheet: high precision, heater off; 8.3 ms maximum.
  // https://sensirion.com/media/documents/33FD6951/6555C40E/Sensirion_Datasheet_SHT4x.pdf
  constexpr uint8_t address = 0x44;
  Wire.beginTransmission(address);
  Wire.write(0xFD);
  if (Wire.endTransmission() != 0) return result;
  delay(10);
  if (Wire.requestFrom(address, static_cast<uint8_t>(6)) != 6) return result;
  uint8_t data[6];
  for (auto& byte : data) {
    const int value = Wire.read();
    if (value < 0) return result;
    byte = static_cast<uint8_t>(value);
  }
  if (!validCrc(data) || !validCrc(data + 3)) return result;
  const uint16_t temperature = (data[0] << 8) | data[1];
  const uint16_t humidity = (data[3] << 8) | data[4];
  result.temperatureC = -45.0f + 175.0f * temperature / 65535.0f;
  result.humidityPercent = -6.0f + 125.0f * humidity / 65535.0f;
  if (result.humidityPercent < 0) result.humidityPercent = 0;
  if (result.humidityPercent > 100) result.humidityPercent = 100;
  result.valid = true;
  return result;
}
