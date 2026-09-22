#pragma once

#include <atomic>
#include <esp_http_client.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "dashboard_protocol.h"
#include "sticky/climate.h"

// One bounded request on a worker. No I2C and no panel access here. Cancellation
// does not join the worker on the voice capture path; its buffer stays owned here.
class Dashboard {
 public:
  bool start(int battery, stickyClimate::Reading climate);
  void cancel();
  bool done() const { return _done.load(std::memory_order_acquire); }
  bool ok() const { return done() && _ok && !_cancelled.load(); }
  const uint8_t* pixels() const { return _pixels; }
  uint64_t receivedRtcTicks() const { return _receivedRtcTicks; }
  uint32_t nextSeconds() const { return _frame.nextSeconds; }
  void poll();
  void close();  // only after done(); keeps the pixel buffer alive for Display

 private:
  static esp_err_t event(esp_http_client_event_t* event);
  static void run(void* self);
  uint8_t* _pixels = nullptr;
  dashboardProtocol::Frame _frame{nullptr};
  esp_http_client_handle_t _client = nullptr;
  std::atomic<bool> _done{true};
  std::atomic<bool> _cancelled{false};
  std::atomic<int> _socket{-1};
  bool _ok = false;
  int64_t _startedUs = 0;
  uint64_t _receivedRtcTicks = 0;
};
