#include "sticky_wifi.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_timer.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace {

// RTC slow memory survives deep sleep and a reset but not a power cycle, which
// is the boundary this cache wants: a board that has been switched off may well
// be somewhere else. The magic is what tells a real entry from whatever the
// section happens to hold on the first boot.
constexpr uint32_t kCacheMagic = 0x571FCA01;

struct ApCache {
  uint32_t magic;
  uint32_t ssid;  // hash of the SSID the entry belongs to
  uint8_t bssid[StickyWifi::kBssidBytes];
  uint8_t channel;
};

RTC_DATA_ATTR ApCache g_ap;

// FNV-1a, so that editing src/secrets.h invalidates the cache by itself. The
// SSID is not stored: a hash is enough to answer "is this entry about the
// network we are being asked to join", and nothing else is ever asked of it.
uint32_t hashOf(const char* text) {
  uint32_t hash = 2166136261u;
  for (; *text != '\0'; ++text) {
    hash ^= static_cast<uint8_t>(*text);
    hash *= 16777619u;
  }
  return hash;
}

bool cacheHolds(uint32_t ssid) {
  return g_ap.magic == kCacheMagic && g_ap.ssid == ssid && g_ap.channel >= 1 &&
         g_ap.channel <= 14;
}

// How long an attempt that is not using the cache gets before it is started
// again from scratch. With config.h's numbers the budget is spent as one cached
// attempt of 3 s and then two fresh scans of 6 s -- 15 s, the timeout exactly.
//
// The window is what decides a retry, rather than WiFi.status() saying the
// attempt has ended, because the status is not a reliable answer to that
// question: it holds the *previous* attempt's terminal value until the new one
// produces an event of its own, so a healthy association would be torn down and
// restarted on the strength of a result that is already history. A window
// several times longer than a connect that is going to work cannot do that.
constexpr uint32_t kScanAttemptMs = 6000;

// How long a WiFi.begin() that refused to start anything waits before it is
// tried again. Short, because nothing is in flight to wait for -- and long
// enough for the library's own uninvited retry to let go of the station, which
// is the likeliest reason it refused.
constexpr uint32_t kRetryDelayMs = 300;

}  // namespace

bool StickyWifi::cachedAp(uint8_t bssid[kBssidBytes], uint8_t& channel) {
  if (g_ap.magic != kCacheMagic) return false;
  memcpy(bssid, g_ap.bssid, kBssidBytes);
  channel = g_ap.channel;
  return true;
}

void StickyWifi::forgetAp() { g_ap.magic = 0; }

bool StickyWifi::begin(const char* ssid, const char* password) {
  if (ssid == nullptr || ssid[0] == '\0') {
    _state = State::Failed;
    _lastError = "no WiFi credentials in src/secrets.h";
    return false;
  }

  _ssid = ssid;
  _password = password;
  _state = State::Connecting;
  _lastError = "";
  _startUs = esp_timer_get_time();
  _settledMs = 0;
  _linkMs = 0;
  _attemptEndsAtMs = 0;
  _retryAtMs = 0;
  _attempts = 0;
  _beginFailed = false;
  _usedCache = false;
  _channel = 0;
  _rssi = 0;
  _ipv4 = 0;
  _ip[0] = '\0';
  memset(_bssid, 0, sizeof(_bssid));

  // None of this belongs in NVS. The credentials are compiled in and the AP
  // lives in RTC memory, so persisting them would be a flash write on every
  // wake in exchange for nothing. Auto-reconnect is off for the same reason the
  // fallback exists: retries are this class's decision, taken against a budget,
  // not the library's taken against nothing.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  _hadCache = cacheHolds(hashOf(ssid));
  startAttempt(_hadCache);
  return true;
}

StickyWifi::State StickyWifi::poll() {
  switch (_state) {
    case State::Idle:
    case State::Failed:
      return _state;

    case State::Online:
      // An association that goes away before the answer has been fetched is a
      // failure like any other, and the orchestrator has to hear about it while
      // the recording is still running.
      if (WiFi.status() != WL_CONNECTED) {
        _state = State::Failed;
        _lastError = "association dropped";
      }
      return _state;

    case State::Connecting:
      break;
  }

  const wl_status_t status = WiFi.status();
  const uint32_t ms = elapsedMs();

  if (status == WL_CONNECTED) {
    _settledMs = ms;
    recordSuccess();
    _state = State::Online;
    return _state;
  }

  // Associated and authenticated, waiting for DHCP. Caught on the way past,
  // because the status leaves this state the moment the address arrives.
  if (_linkMs == 0 && status == WL_IDLE_STATUS) _linkMs = ms;

  if (ms >= config::kWifiConnectTimeoutMs) {
    _settledMs = ms;
    _state = State::Failed;
    _lastError = "no association inside the budget";
    // An entry that has just cost a full timeout is worth less than no entry:
    // the next wake should scan rather than pay for it a second time.
    forgetAp();
    return _state;
  }

  // The attempt in flight has had its window. For the cached one that means the
  // cache has had its chance and the scan the firmware would have done without
  // a cache starts instead; for a scan it means starting over. Either way it is
  // inside the same budget, so the fallback costs the user no time beyond what
  // the cache was given.
  //
  // **Only while there is no link.** A window is for an association that is not
  // happening; once the link is up the attempt has done the hard part and is
  // waiting for DHCP, which on this network is seconds (S6 in
  // docs/implementation.md) and is not made faster by throwing the association
  // away and doing it again. The first run of the S6 driver did exactly that
  // and turned a 3.9 s connect into a 6.3 s one. Past that point the budget is
  // the only thing left that can end the attempt.
  //
  // A begin() that refused to start anything is the one case that does not wait
  // for a window: there is nothing in flight to disturb.
  if (_linkMs == 0 && (ms >= _attemptEndsAtMs || (_beginFailed && ms >= _retryAtMs))) {
    startAttempt(false);
  }

  return _state;
}

void StickyWifi::end() {
  WiFi.disconnect(true, false, 0);  // stop the station, keep the config
  _state = State::Idle;
}

uint32_t StickyWifi::elapsedMs() const {
  if (_state == State::Idle) return 0;
  if (_settledMs != 0) return _settledMs;
  return static_cast<uint32_t>((esp_timer_get_time() - _startUs) / 1000);
}

void StickyWifi::startAttempt(bool useCache) {
  // Whatever the library still has in flight has to go first, or
  // esp_wifi_connect() refuses the new attempt -- and after the first failure
  // there is always something in flight, because WiFiSTA retries once on its
  // own whatever setAutoReconnect() was told.
  if (_attempts > 0) WiFi.disconnect(false, false, 0);

  const uint32_t ms = elapsedMs();
  _usedCache = useCache;
  ++_attempts;
  _attemptEndsAtMs = ms + (useCache ? config::kWifiCachedAttemptMs : kScanAttemptMs);
  _retryAtMs = ms + kRetryDelayMs;

  // WiFi.begin() reports a refusal as WL_CONNECT_FAILED, but on the way out it
  // also returns whatever the status already was -- and WL_CONNECT_FAILED is a
  // status a previous attempt can leave behind. Only a value that was not there
  // before the call is news, or a wrong password would have this restarting
  // every kRetryDelayMs for the whole budget.
  const wl_status_t before = WiFi.status();
  const wl_status_t started = useCache
                                  ? WiFi.begin(_ssid, _password, g_ap.channel, g_ap.bssid)
                                  : WiFi.begin(_ssid, _password);
  _beginFailed = (started == WL_CONNECT_FAILED && before != WL_CONNECT_FAILED);
}

void StickyWifi::recordSuccess() {
  WiFi.BSSID(_bssid);
  _channel = static_cast<uint8_t>(WiFi.channel());
  _rssi = WiFi.RSSI();

  const IPAddress ip = WiFi.localIP();
  _ipv4 = static_cast<uint32_t>(ip);
  snprintf(_ip, sizeof(_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

  // The AP that just worked, for the next wake. Written on every success rather
  // than only when the cache was empty, so a network that moved channel fixes
  // itself in one question instead of staying wrong.
  g_ap.magic = kCacheMagic;
  g_ap.ssid = hashOf(_ssid);
  memcpy(g_ap.bssid, _bssid, kBssidBytes);
  g_ap.channel = _channel;
}
