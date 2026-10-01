#include "reminders.h"
#include "reminder_json.h"
#include "config.h"
#include "secrets.h"
#include <esp_attr.h>
#include <esp_private/esp_clk.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <soc/rtc.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <atomic>
#include <string>

namespace {
RTC_DATA_ATTR reminder::State saved;
RTC_DATA_ATTR uint64_t anchorTicks = 0;
RTC_DATA_ATTR int64_t anchorMs = 0;
std::atomic<bool> busy{false}, cancelled{false}, done{true};
reminder::Snapshot received;
bool valid = false;
uint64_t receivedTicks = 0;
int64_t ageMs = 0, startedUs = 0;
char requestBody[256];
// Worst case: every byte of 10 x 240-byte text escaped as six JSON bytes,
// plus fields and acknowledgements. Reject oversize instead of truncating.
char responseBody[20000];
void worker(void*) {
  valid = false;
  const std::string url = std::string(secrets::kBackendBaseUrl) + "/sticky/reminders/sync";
  esp_http_client_config_t cfg{};
  cfg.url = url.c_str(); cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = config::kBackendConnectTimeoutMs;
  cfg.disable_auto_redirect = true;
  auto client = esp_http_client_init(&cfg);
  if (client) {
    const std::string auth = std::string("Bearer ") + secrets::kDeviceToken;
    esp_http_client_set_header(client, "Authorization", auth.c_str());
    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_header(client, "Connection", "close");
    const int length = strlen(requestBody);
    if (!cancelled && esp_http_client_open(client, length) == ESP_OK) {
      if (!cancelled && esp_http_client_write(client, requestBody, length) == length) {
        int64_t size = esp_http_client_fetch_headers(client);
        if (!cancelled && size >= 0 && size < sizeof(responseBody) && esp_http_client_get_status_code(client) == 200) {
          esp_http_client_set_timeout_ms(client, 500);
          size_t got = 0;
          while (!cancelled && got < sizeof(responseBody) - 1 &&
                 esp_timer_get_time() - startedUs < config::kDashboardRequestMs * 1000LL &&
                 !esp_http_client_is_complete_data_received(client)) {
            int n = esp_http_client_read(client, responseBody + got, sizeof(responseBody) - 1 - got);
            if (n == -ESP_ERR_HTTP_EAGAIN) continue;
            if (n <= 0) break;
            got += n;
          }
          receivedTicks = rtc_time_get();
          ageMs = (esp_timer_get_time() - startedUs) / 2000;
          if (!cancelled && esp_http_client_is_complete_data_received(client)) {
            JsonDocument doc;
            valid = !deserializeJson(doc, responseBody, got) && reminder::parse(doc.as<JsonVariantConst>(), received);
          }
        }
      }
    }
    // Only this worker owns cleanup. Cancellation uses a lock-free flag;
    // reads are bounded, so no descriptor can be shut down after reuse.
    esp_http_client_cleanup(client);
  }
  done.store(true, std::memory_order_release);
  vTaskDelete(nullptr);
}
}
void reminders::begin(bool cold) {
  if (cold) { saved = reminder::State{}; anchorMs = 0; anchorTicks = 0; }
}
reminder::State& reminders::state() { return saved; }
int64_t reminders::nowMs() {
  if (!anchorMs) return 0;
  return anchorMs + rtc_time_slowclk_to_us(rtc_time_get() - anchorTicks, esp_clk_slowclk_cal_get()) / 1000;
}
bool reminders::due() { return anchorMs && saved.due(nowMs()); }
uint64_t reminders::sleepUs(uint64_t other) {
  if (!anchorMs || saved.nextDue() == reminder::kMaxTime) return other;
  const int64_t left = saved.nextDue() - nowMs();
  const uint64_t us = left > 0 ? left * 1000ULL : 1000;
  return us < other ? us : other;
}
bool reminders::accept(JsonVariantConst json, uint64_t ticks, int64_t age) {
  static reminder::Snapshot s;
  if (!reminder::parse(json, s) || !saved.apply(s)) return false;
  anchorTicks = ticks; anchorMs = s.serverMs + age;
  return true;
}
void reminders::startSync() {
  if (busy || !secrets::kBackendBaseUrl[0]) return;
  JsonDocument doc;
  auto ids = doc["read_ids"].to<JsonArray>();
  for (size_t i = 0; i < saved.pendingCount; ++i) ids.add(saved.pending[i]);
  serializeJson(doc, requestBody, sizeof(requestBody));
  cancelled = false; done = false; busy = true; startedUs = esp_timer_get_time();
  if (xTaskCreate(worker, "reminders", 12288, nullptr, 1, nullptr) != pdPASS) { done = true; busy = false; }
}
void reminders::pollSync() {
  if (!busy) return;
  if (!done.load(std::memory_order_acquire)) {
    if (esp_timer_get_time() - startedUs >= config::kDashboardRequestMs * 1000LL) cancelled = true;
    return;
  }
  if (valid && !cancelled && saved.apply(received)) {
    anchorTicks = receivedTicks; anchorMs = received.serverMs + ageMs;
  }
  busy = false;
}
void reminders::cancelSync() { cancelled = true; }
bool reminders::syncing() { return busy; }
