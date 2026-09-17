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
// **The address is cached next to the AP, and it is the larger saving of the
// two.** The association was never the expensive half: the link comes up in
// 87 ms and DHCP behind it takes 3.2 s, of which 2.1 s is the router taking its
// time over the OFFER and a full second is lwIP's ARP check -- docs/
// experiments.md, E6. So the lease the last wake was given is kept in RTC
// memory and installed with WiFi.config() before the association, which skips
// the client altogether and puts the device on a usable network about 200 ms
// after the top of setup().
//
// Nothing here is a claim on a fixed address. The only lease the device ever
// installs is one this network handed it, and it is used for less than half the
// life the server gave it -- which is DHCP's own T1, the point at which a client
// that could renew would be renewing. Past that it asks properly.
//
// **A lease that has outlived its network cannot be detected from here.** A
// stale address installs in the same 40 ms as a good one and the association
// says nothing about it; the first thing that notices is a packet that goes
// nowhere. So the detector lives where the first packet is -- the orchestrator's
// upload -- and this class provides the other half of the rule: renewAddress()
// throws the lease away and asks the server, which costs the question that
// noticed one DHCP exchange and every question after it nothing.
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
//    DHCP's reply, which is why linkMs() and onlineMs() are reported apart:
//    the gap between them is DHCP and nothing else. It is also almost all of a
//    connect -- 3.15 s against an 87 ms link on the network this was measured
//    on, which is docs/experiments.md, E6.
//  * **The first failed attempt is retried once by the library**, whatever
//    setAutoReconnect() was told (WiFiSTA's `first_connect` static). It reuses
//    the config it was given, cached BSSID included, so a stale cache costs two
//    attempts rather than one -- which is the reason the fallback is decided on
//    a clock rather than on a status code.
class WifiLink {
 public:
  enum class State : uint8_t {
    Idle,        // begin() has not been called, or end() has
    Connecting,  // an attempt is in flight
    Online,      // associated with an IP
    Failed,      // out of budget, or the association dropped afterwards
  };

  static constexpr uint32_t kBssidBytes = 6;

  // An address the server gave this device, as it survives the sleep. Held as
  // raw uint32_t in IPAddress's own byte order -- first octet in the low byte --
  // so that nothing outside the .cpp has to include WiFi.h to read it.
  struct Lease {
    uint32_t ip;
    uint32_t gateway;
    uint32_t mask;
    uint32_t dns;
    uint32_t seconds;  // the life the server offered, from the lwIP client
    uint32_t ageS;     // how long ago it was granted, across however many sleeps
  };

  // What the last successful connect left in RTC memory, for a log line before
  // the attempt starts. False when there is nothing cached, which is every
  // cold start.
  static bool cachedAp(uint8_t bssid[kBssidBytes], uint8_t& channel);
  static bool cachedLease(Lease& lease);

  // Drops the cache, so the next begin() scans. Called for the caller when an
  // attempt fails outright -- an entry that has just cost a full timeout is
  // worse than no entry.
  static void forgetAp();

  // Drops the address, so the next begin() asks for one. renewAddress() calls
  // it; the orchestrator does not have to.
  static void forgetLease();

  // Moves the cached lease onto a subnet the device is not on, which is what a
  // lease that has outlived its network looks like from here: it installs
  // perfectly and reaches nothing. **Nothing in the firmware calls this.** It
  // exists because the stale-lease path is the one thing in this class that
  // cannot be reached by waiting, and the S7b driver has to be able to walk it
  // on demand. False when there was no lease to spoil.
  static bool spoilLease();

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

  // Throws the installed lease away and asks the server for an address, after a
  // question that reached nothing at all. Drops the RTC entry with it, so a
  // wake that ends badly anyway does not leave the same address behind for the
  // next one.
  //
  // The state goes back to Connecting and poll() drives it exactly as before,
  // against the same config::kWifiConnectTimeoutMs -- an address is part of an
  // association and gets the association's budget. Only meaningful while
  // online(); anything else is ignored.
  void renewAddress();

  // Stops the radio. Deep sleep would take it down anyway, but the IDF asks for
  // the station to be stopped before the chip goes, and doing it here means the
  // last thing before sleep is the latch and nothing else.
  void end();

  // Why the attempt failed, for Serial1. The screen gets the vision's NO WIFI
  // instead -- these strings are for whoever is reading the log.
  const char* lastError() const { return _lastError; }

  // begin() to now, or to the moment the attempt settled. It is the caller's
  // own view: the attempt settles when a poll notices, so a caller that stops
  // polling to do something slow gets that something back in this number.
  uint32_t elapsedMs() const;

  // The two moments the radio reached, timed by the WiFi task rather than by
  // whoever is polling: the link, and the address behind it. Both are begin()
  // to the event, and both are zero until it happens.
  //
  // **They are separate from elapsedMs() because polling is not measurement.**
  // The orchestrator spends two and a half seconds inside a panel refresh
  // between one poll and the next, and an address that arrives 40 ms in is not
  // noticed until that is over -- which is how S7b's first run came to report
  // an installed lease as costing 2.6 s, to the millisecond, four wakes
  // running. Anything comparing this firmware against E6's rig has to use these
  // two; elapsedMs() answers a different question and is right about it.
  uint32_t linkMs() const;
  uint32_t onlineMs() const;

  // Whether the attempt that is running, or the one that won, used the cached
  // AP; and whether there was a cache to try in the first place.
  bool usedCache() const { return _usedCache; }
  bool hadCache() const { return _hadCache; }

  // The same two questions about the address. hadLease() is about the entry --
  // there was one, for this network, whatever its age. usedLease() is about the
  // address in hand, and goes false again the moment renewAddress() drops it:
  // it answers "is what this wake is running on a reused address" rather than
  // "did this wake start with one".
  bool usedLease() const { return _usedLease; }
  bool hadLease() const { return _hadLease; }

  // The life the server put on the address this wake is running on. Zero after
  // a DHCP wake whose client never said, which is the one way the cache can
  // quietly fail to fill -- how old the entry is belongs to cachedLease(),
  // which is read before begin() and is where the log line comes from.
  uint32_t leaseSeconds() const { return _leaseSeconds; }

  // renewAddress() to the address in hand. Zero until one has been asked for.
  uint32_t renewMs() const { return _renewMs; }

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

  // The second half of poll(), for the wakes that have thrown a lease away.
  // Separate because it waits on a different witness: the DHCP client's own
  // state rather than WiFi.status(), which past a first success is history.
  State pollRenew();

  // Reads what the association ended up with and writes the AP -- and, when the
  // address came from the server rather than from the cache, the lease -- to
  // RTC memory.
  void recordSuccess();

  const char* _ssid = nullptr;
  const char* _password = nullptr;

  State _state = State::Idle;
  const char* _lastError = "";

  int64_t _startUs = 0;
  uint32_t _settledMs = 0;  // elapsed at the moment the attempt stopped moving
  uint32_t _attemptEndsAtMs = 0;
  uint32_t _retryAtMs = 0;

  bool _hadCache = false;
  bool _usedCache = false;
  bool _beginFailed = false;
  uint32_t _attempts = 0;

  bool _hadLease = false;
  bool _usedLease = false;
  uint32_t _leaseSeconds = 0;

  bool _renewing = false;
  int64_t _renewStartUs = 0;
  uint32_t _renewMs = 0;

  uint8_t _bssid[kBssidBytes] = {0};
  uint8_t _channel = 0;
  int8_t _rssi = 0;
  uint32_t _ipv4 = 0;
  char _ip[16] = {0};
};
