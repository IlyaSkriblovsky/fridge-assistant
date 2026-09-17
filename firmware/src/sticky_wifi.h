#pragma once

#include <stdint.h>

// The WiFi association, as something the orchestrator drives rather than waits
// for.
//
// Everything here follows from one rule in the vision: the orchestrator must
// never sit inside a call while the button is being released. WiFi.begin()
// hands the work to the IDF's own tasks on core 0 and returns, so the
// association makes progress by itself; what would break the rule is
// waitForConnectResult(), which parks this task for as long as the AP takes.
// So the class is begin() plus a poll() that is cheap enough to call from a
// loop that is also refreshing the panel.
//
// **The AP is cached in RTC memory across the sleep** -- BSSID and channel, so
// the next wake connects to a known radio on a known channel instead of
// scanning all of them. The vision counts this as load-bearing rather than an
// optimisation: reconnect time is what the 30 s recording buffer is covering,
// and every question pays it. The cache survives deep sleep and a reset but not
// a power cycle, which is the right boundary -- a board that has been off may
// well be somewhere else.
//
// A cached attempt can be wrong: the AP may have moved channel, or be a
// different radio of the same network, or be gone. So it is time-boxed to
// config::kWifiCachedAttemptMs and then the attempt starts again with a plain
// scan, which is what the firmware would have done without a cache at all. Both
// halves share one deadline, config::kWifiConnectTimeoutMs, so NO WIFI arrives
// at a predictable time whichever path it took.
//
// **The time-box only runs while there is no link.** It exists for an
// association that is not happening; once the link is up, what is left is DHCP,
// and starting over would throw away the part that worked. Past the link only
// the deadline can end the attempt.
//
// Two things about arduino-esp32's WiFi are worth knowing before reading a log
// from this module:
//
//  * **WL_CONNECTED means an IP, not an association.** The status goes to
//    WL_IDLE_STATUS when the link comes up and only reaches WL_CONNECTED on
//    DHCP's reply, which is why linkMs() and elapsedMs() are reported apart:
//    the gap between them is DHCP and nothing else. It is also almost all of a
//    connect -- 3.15 s against an 87 ms link on the network this was measured
//    on, which is docs/experiments.md, E6.
//  * **The first failed attempt is retried once by the library**, whatever
//    setAutoReconnect() was told (WiFiSTA's `first_connect` static). It reuses
//    the config it was given, cached BSSID included, so a stale cache costs two
//    attempts rather than one -- which is the reason the fallback is decided on
//    a clock rather than on a status code.
class StickyWifi {
 public:
  enum class State : uint8_t {
    Idle,        // begin() has not been called, or end() has
    Connecting,  // an attempt is in flight
    Online,      // associated with an IP
    Failed,      // out of budget, or the association dropped afterwards
  };

  static constexpr uint32_t kBssidBytes = 6;

  // What the last successful connect left in RTC memory, for a log line before
  // the attempt starts. False when there is nothing cached, which is every
  // cold start.
  static bool cachedAp(uint8_t bssid[kBssidBytes], uint8_t& channel);

  // Drops the cache, so the next begin() scans. Called for the caller when an
  // attempt fails outright -- an entry that has just cost a full timeout is
  // worse than no entry.
  static void forgetAp();

  // Starts the association and returns immediately. False means nothing was
  // started: no credentials, which is a build-time mistake rather than a
  // network failure, and lastError() says so.
  bool begin(const char* ssid, const char* password);

  // Advances the attempt and reports where it is. Cheap: one WiFi.status() and
  // some arithmetic, no blocking, safe to call from a loop that is also doing
  // something slow.
  //
  // Once Online, it keeps watching: an association that drops before the answer
  // has been fetched turns back into Failed, which is the vision's rule that a
  // recording with nowhere to go is aborted rather than finished.
  State poll();

  State state() const { return _state; }
  bool online() const { return _state == State::Online; }
  bool failed() const { return _state == State::Failed; }

  // Stops the radio. Deep sleep would take it down anyway, but the IDF asks for
  // the station to be stopped before the chip goes, and doing it here means the
  // last thing before sleep is the latch and nothing else.
  void end();

  // Why the attempt failed, for Serial1. The screen gets the vision's NO WIFI
  // instead -- these strings are for whoever is reading the log.
  const char* lastError() const { return _lastError; }

  // begin() to now, or to the moment the attempt settled.
  uint32_t elapsedMs() const;

  // When the link came up, i.e. associated and authenticated but without an
  // address yet. Zero when the poll never caught that state, which happens when
  // DHCP answers inside one poll interval -- it is an observation at the
  // caller's polling resolution, not an event timestamp.
  uint32_t linkMs() const { return _linkMs; }

  // Whether the attempt that is running, or the one that won, used the cached
  // AP; and whether there was a cache to try in the first place.
  bool usedCache() const { return _usedCache; }
  bool hadCache() const { return _hadCache; }

  // How many times WiFi.begin() was called this wake. One is the happy path;
  // two is the fallback after a cached attempt; more means the library refused
  // to start an attempt and it was tried again.
  uint32_t attempts() const { return _attempts; }

  // Meaningful once online(). The BSSID and channel are also what went into the
  // cache, so a log line from them is a log line about the next wake too.
  const uint8_t* bssid() const { return _bssid; }
  uint8_t channel() const { return _channel; }
  int8_t rssi() const { return _rssi; }
  uint32_t ipv4() const { return _ipv4; }
  const char* ip() const { return _ip; }

 private:
  // Calls WiFi.begin(), with the cached AP or without it. Everything that
  // decides *when* to do that is in poll().
  void startAttempt(bool useCache);

  // Reads what the association ended up with and writes the AP to RTC memory.
  void recordSuccess();

  const char* _ssid = nullptr;
  const char* _password = nullptr;

  State _state = State::Idle;
  const char* _lastError = "";

  int64_t _startUs = 0;
  uint32_t _settledMs = 0;  // elapsed at the moment the attempt stopped moving
  uint32_t _linkMs = 0;
  uint32_t _attemptEndsAtMs = 0;
  uint32_t _retryAtMs = 0;

  bool _hadCache = false;
  bool _usedCache = false;
  bool _beginFailed = false;
  uint32_t _attempts = 0;

  uint8_t _bssid[kBssidBytes] = {0};
  uint8_t _channel = 0;
  int8_t _rssi = 0;
  uint32_t _ipv4 = 0;
  char _ip[16] = {0};
};
