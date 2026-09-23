// E2: identical production Backend requests over HTTP and verified HTTPS.
// Link wrapping adds trust only to this environment; production is unchanged.
#include <Arduino.h>
#include <WiFi.h>
#include <esp_crt_bundle.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <time.h>

#include "backend.h"
#include "recording.h"
#include "secrets.h"
#include "sticky/power.h"
#include "wifi_link.h"

namespace {
constexpr unsigned kPairsPerShape = 12;
constexpr unsigned kCycles = kPairsPerShape * 4;
RTC_DATA_ATTR unsigned cycle = 0;
int64_t transportConnectedUs = 0;
Recording audio;
WifiLink wifi;
Backend backend(secrets::kBackendBaseUrl, secrets::kDeviceToken);

esp_err_t event(esp_http_client_event_t* e) {
  if (e->event_id == HTTP_EVENT_ON_CONNECTED)
    transportConnectedUs = esp_timer_get_time();
  return ESP_OK;
}

[[noreturn]] void halt(const char* why) {
  Serial1.printf("E2 FAILED %s\n", why);
  Serial1.flush();
  wifi.end();
  stickyPower::deepSleep();
}
}  // namespace

extern "C" esp_http_client_handle_t __real_esp_http_client_init(
    const esp_http_client_config_t*);
extern "C" esp_http_client_handle_t __wrap_esp_http_client_init(
    const esp_http_client_config_t* original) {
  esp_http_client_config_t cfg = *original;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  cfg.event_handler = event;
  return __real_esp_http_client_init(&cfg);
}

void setup() {
  stickyPower::holdLatch();
  Serial1.begin(115200, SERIAL_8N1, 44, 43);
  delay(100);
  if (!stickyPower::wokeFromDeepSleep()) cycle = 0;
  if (cycle >= kCycles) halt("finished; reset to repeat");

  // Four-wake blocks: tiny HTTP/HTTPS, streamed HTTPS/HTTP, then reverse
  // the order in the next block. One request per deep-sleep wake, no ticket
  // or TLS context survives. The first power-on row is labelled in the log.
  const unsigned block = cycle / 4;
  const unsigned slot = cycle % 4;
  const bool streamed = slot >= 2;
  const bool tls = ((slot % 2) ^ (block % 2) ^ unsigned(streamed)) != 0;
  const char* host = strstr(secrets::kBackendBaseUrl, "://");
  if (!host) halt("backend URL needs scheme");
  host += 3;
  // This rig compares standard ports on the same public hostname.
  if (!*host || strchr(host, ':') || strchr(host, '/'))
    halt("backend URL must contain a hostname without port or trailing slash");
  char url[160];
  snprintf(url, sizeof(url), "%s://%s/audio/fault/500", tls ? "https" : "http", host);

  if (!audio.begin(16000)) halt(audio.lastError());
  if (streamed) {
    for (unsigned n = 0; n < 250; ++n) {
      memset(audio.writeHead(), 0, 512);
      audio.commit(256);
    }
  }
  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword, secrets::kDnsServer))
    halt(wifi.lastError());
  while (wifi.poll() == WifiLink::State::Connecting) delay(1);
  if (!wifi.online()) halt(wifi.lastError());

  // Establish real time once for certificate validity; RTC retains it through
  // deep sleep. Clock setup is logged separately, outside request timing.
  const int64_t clockStart = esp_timer_get_time();
  if (time(nullptr) < 1789430400) {
    configTime(0, 0, "pool.ntp.org", "time.cloudflare.com");
    while (time(nullptr) < 1789430400 &&
           esp_timer_get_time() - clockStart < 20000000) delay(10);
    if (time(nullptr) < 1789430400) halt("SNTP clock unavailable");
  }
  Serial1.printf("E2 START cycle=%u scheme=%s shape=%s wake=%s wifi_ms=%lu clock_ms=%lld rssi=%d epoch=%lld\n",
                 cycle, tls ? "https" : "http", streamed ? "stream" : "tiny",
                 stickyPower::wakeupCauseName(), (unsigned long)wifi.onlineMs(),
                 (esp_timer_get_time() - clockStart) / 1000, WiFi.RSSI(), (long long)time(nullptr));
  Serial1.flush();

  const unsigned heapBefore = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  heap_caps_monitor_local_minimum_free_size_start();
  bool ok = backend.open(url);
  const int64_t uploadStart = esp_timer_get_time();
  int64_t releaseUs = uploadStart;
  if (ok && !streamed) ok = backend.write(audio.wav(), audio.wavBytes());
  if (ok && streamed) {
    ok = backend.write(audio.wav(), Recording::kHeaderBytes);
    // 4 seconds of generated PCM at the actual 512 bytes / 16 ms cadence.
    for (unsigned n = 0; ok && n < 250; ++n) {
      const int64_t due = uploadStart + (n + 1) * 16000LL;
      while (esp_timer_get_time() < due) delay(1);
      ok = backend.write(audio.wav() + Recording::kHeaderBytes + n * 512, 512);
    }
    releaseUs = uploadStart + 4000000;
  } else {
    releaseUs = esp_timer_get_time();
  }
  if (ok) ok = backend.end();
  if (ok) backend.receive(); // No chirp or display before reading headers.
  const unsigned heapMin = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  heap_caps_monitor_local_minimum_free_size_stop();
  const auto delta = [](int64_t end, int64_t start) -> long long {
    return end ? end - start : -1;
  };
  Serial1.printf("E2 ROW,%u,%s,%s,%d,%u,%lld,%lld,%lld,%lld,%lld,%lu,%u,%u\n",
      cycle, tls ? "https" : "http", streamed ? "stream" : "tiny",
      backend.status(), (unsigned)backend.sentBytes(),
      delta(transportConnectedUs, backend.openUs()),
      delta(backend.connectedUs(), backend.openUs()),
      delta(backend.firstByteUs(), backend.openUs()),
      delta(backend.firstByteUs(), backend.endUs()),
      delta(backend.firstByteUs(), releaseUs),
      (unsigned long)backend.longestWriteUs(), heapBefore, heapMin);
  if (backend.status() != 500) Serial1.printf("E2 ERROR %s\n", backend.lastError());
  ++cycle;
  if (cycle == kCycles) Serial1.println("E2 DONE");
  Serial1.flush();
  wifi.end();
  stickyPower::deepSleep(cycle < kCycles ? 1000000 : 0);
}

void loop() {}
