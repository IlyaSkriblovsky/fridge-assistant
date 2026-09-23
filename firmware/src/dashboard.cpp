#include "dashboard.h"
#include "dashboard_png.h"

#include <esp_heap_caps.h>
#include <soc/rtc.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <string>
#include "secrets.h"

bool Dashboard::start(int battery, stickyClimate::Reading climate) {
  if (!done()) return false;
  close();
  _ok = false;
  _requestMs = 0;
  _decodeMs = 0;
  _cancelled = false;
  if (!_pixels) _pixels = static_cast<uint8_t*>(heap_caps_malloc(
      dashboardProtocol::kFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_pixels || !secrets::kBackendBaseUrl[0]) return false;
  _encoded = static_cast<uint8_t*>(heap_caps_malloc(
      dashboardProtocol::kMaxPngBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (!_encoded) return false;
  _frame = dashboardProtocol::Frame{_encoded};
  std::string url = std::string(secrets::kBackendBaseUrl) + config::kDashboardPath;
  char separator = '?';
  char value[100];
  if (battery >= 0 && battery <= 100) {
    snprintf(value, sizeof(value), "%cbattery_pct=%d", separator, battery);
    url += value;
    separator = '&';
  }
  if (climate.valid) {
    snprintf(value, sizeof(value), "%ctemperature_c=%.1f&humidity_pct=%.1f",
             separator, climate.temperatureC, climate.humidityPercent);
    url += value;
  }
  esp_http_client_config_t cfg = {};
  cfg.url = url.c_str();
  cfg.method = HTTP_METHOD_GET;
  cfg.timeout_ms = config::kBackendConnectTimeoutMs;
  cfg.disable_auto_redirect = true;
  cfg.event_handler = event;
  cfg.user_data = this;
  _client = esp_http_client_init(&cfg);
  if (!_client) return false;
  const std::string auth = std::string("Bearer ") + secrets::kDeviceToken;
  esp_http_client_set_header(_client, "Authorization", auth.c_str());
  esp_http_client_set_header(_client, "Accept-Encoding", "identity");
  esp_http_client_set_header(_client, "Connection", "close");
  _startedUs = esp_timer_get_time();
  _done.store(false, std::memory_order_release);
  if (xTaskCreate(run, "dashboard", 6144, this, 1, nullptr) != pdPASS) {
    _done.store(true, std::memory_order_release);
    return false;
  }
  return true;
}

void Dashboard::cancel() {
  _cancelled.store(true);
  // Publish only after open() succeeds. Manual fetch/read never closes this
  // socket: close() owns that, after completion, on the orchestrator. shutdown
  // interrupts a read without freeing/reusing the descriptor or HTTP state.
  // esp_http_client_cancel_request() also prepares a new request, so is not
  // safe concurrently with a parser reading the old one.
  const int socket = _socket.load();
  if (!done() && socket >= 0) shutdown(socket, SHUT_RDWR);
}

void Dashboard::close() {
  if (!done()) return;
  _socket = -1;
  if (_client) { esp_http_client_cleanup(_client); _client = nullptr; }
  heap_caps_free(_encoded);
  _encoded = nullptr;
  _frame.data = nullptr;
}

void Dashboard::poll() {
  if (!done() && !_cancelled.load() &&
      esp_timer_get_time() - _startedUs >= config::kDashboardRequestMs * 1000LL) cancel();
}

esp_err_t Dashboard::event(esp_http_client_event_t* event) {
  auto& self = *static_cast<Dashboard*>(event->user_data);
  if (event->event_id == HTTP_EVENT_ON_HEADER)
    self._frame.header(event->header_key, event->header_value);
  return ESP_OK;
}

void Dashboard::run(void* context) {
  auto& self = *static_cast<Dashboard*>(context);
  auto client = self._client;
  bool valid = false;
  // Start on the worker so task scheduling and WiFi startup are not counted.
  const int64_t requestUs = esp_timer_get_time();
  if (!self._cancelled.load() && esp_http_client_open(client, 0) == ESP_OK) {
    self._socket = esp_http_client_get_socket(client);
    const int64_t length = self._cancelled.load() ? -1 : esp_http_client_fetch_headers(client);
    if (!self._cancelled.load() && length > 0 && length <= dashboardProtocol::kMaxPngBytes &&
        esp_http_client_get_status_code(client) == 200 && !self._frame.bad &&
        self._frame.format && self._frame.type &&
        !esp_http_client_is_chunked_response(client)) {
      esp_http_client_set_timeout_ms(client, 500);
      char chunk[1024];
      while (!self._cancelled.load() && !esp_http_client_is_complete_data_received(client) &&
             esp_timer_get_time() - self._startedUs < config::kDashboardRequestMs * 1000LL) {
        const int n = esp_http_client_read(client, chunk, sizeof(chunk));
        if (n == -ESP_ERR_HTTP_EAGAIN) continue;
        if (n <= 0 || !self._frame.append(chunk, n)) break;
      }
      const int64_t receivedUs = esp_timer_get_time();
      valid = receivedUs - self._startedUs < config::kDashboardRequestMs * 1000LL &&
              self._frame.valid(esp_http_client_get_status_code(client), length,
                               esp_http_client_is_complete_data_received(client));
      if (valid) {
        self._requestMs = static_cast<uint32_t>((receivedUs - requestUs) / 1000);
        self._receivedRtcTicks = rtc_time_get();
        const int64_t decodeUs = esp_timer_get_time();
        valid = dashboardPng::decode(self._encoded, self._frame.size, self._pixels,
            [](void* context) {
              auto& dashboard = *static_cast<Dashboard*>(context);
              return dashboard._cancelled.load() ||
                  esp_timer_get_time() - dashboard._startedUs >= config::kDashboardRequestMs * 1000LL;
            }, &self);
        self._decodeMs = static_cast<uint32_t>((esp_timer_get_time() - decodeUs) / 1000);
      }
    }
  }
  self._ok = valid;
  // Cleanup happens on the orchestrator, after this release, never concurrent
  // with cancel(). Nothing below the store touches this object.
  self._done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
