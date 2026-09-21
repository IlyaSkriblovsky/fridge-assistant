#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Minimal RAM bus model for the real Sticky driver. Two different dirty planes
// represent controller RAM after power loss. No waveform or timing simulation.
struct FakeEpaperBus {
  uint8_t command = 0;
  std::vector<uint8_t> oldRam = std::vector<uint8_t>(48000, 0x35);
  std::vector<uint8_t> newRam = std::vector<uint8_t>(48000, 0xca);
  int x0 = 0, y0 = 0, x1 = 99, y1 = 479, x = 0, y = 0;
  size_t imageBytes = 0;

  void window(int xs, int ys, int xe, int ye) {
    x = x0 = xs / 8;
    y = y0 = ys;
    x1 = xe / 8;
    y1 = ye;
  }
  void writeCommand(uint8_t value) { command = value; }
  void writeData(uint8_t value) {
    if (command != 0x24 && command != 0x26) return;
    auto& ram = command == 0x24 ? newRam : oldRam;
    ram.at(y * 100 + x) = value;
    ++imageBytes;
    if (++x > x1) {
      x = x0;
      if (++y > y1) y = y0;
    }
  }
  void writeData(const uint8_t* data, size_t length) {
    while (length--) writeData(*data++);
  }
};

// Only the base-driver surface used by sticky/epaper.h. All inversion, shadow
// bookkeeping and controller seeding are provided by the production subclass.
class Driver_SSD1677 {
 public:
  Driver_SSD1677(uint16_t, uint16_t, int8_t) {}
  virtual ~Driver_SSD1677() = default;
  virtual const char* name() const { return "stub"; }
  virtual void sleep() {}
  virtual void wakePartial() {}
  virtual void updatePartial() { ++updates; }
  virtual void pushOldColors(const uint8_t*, size_t) {}
  virtual void pushNewColors(const uint8_t*, size_t) {}
  virtual void setAddrWindow(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye) {
    bus.window(xs, ys, xe, ye);
  }

  FakeEpaperBus bus;
  FakeEpaperBus* _bus = &bus;
  unsigned updates = 0;
};
