#include "wifi_link.h"

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_private/esp_clk.h>
#include <esp_timer.h>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/prot/dhcp.h>  // the DHCP_STATE_* names; lwip/dhcp.h has only the struct
#include <soc/rtc.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace {

// What tells a real entry from whatever the section happens to hold on the
// first boot.
constexpr uint32_t kCacheMagic = 0x571FCA01;

struct ApCache {
  uint32_t magic;
  uint32_t ssid;  // hash of the SSID the entry belongs to
  uint8_t bssid[WifiLink::kBssidBytes];
  uint8_t channel;
};

// The address, in the same memory and on the same terms. It is a separate entry
// rather than a wider ApCache because the two are dropped for different reasons
// and at different moments: an association that failed says nothing about the
// address, and an address that reaches nothing says nothing about the radio.
//
// The SSID is what it is tied to, not the BSSID: a lease belongs to the network
// behind the access point, so the same subnet reached through a different radio
// of the same network is still the right subnet.
struct LeaseCache {
  uint32_t magic;
  uint32_t ssid;
  uint32_t ip;
  uint32_t gateway;
  uint32_t mask;
  uint32_t dns;
  uint32_t seconds;  // the life the server offered
  uint64_t ticks;    // the RTC counter when it was granted
};

RTC_DATA_ATTR ApCache g_ap;
RTC_DATA_ATTR LeaseCache g_lease;

// When the radio got there, as opposed to when anyone looked: both are written
// from the Arduino event task, so a caller that is busy between two polls does
// not end up in them.
//
// File-static rather than members because the callback carries no context of
// its own, and there is one station on this chip and one class in front of it.
volatile int64_t g_linkUs = 0;
volatile int64_t g_gotIpUs = 0;

void onWifiEvent(arduino_event_id_t event) {
  const int64_t now = esp_timer_get_time();
  if (event == ARDUINO_EVENT_WIFI_STA_CONNECTED) {
    if (g_linkUs == 0) g_linkUs = now;  // the first association of this wake
  } else if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    g_gotIpUs = now;  // ... and the latest address, which a renewal replaces
  }
}

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

bool apCacheHolds(uint32_t ssid) {
  return g_ap.magic == kCacheMagic && g_ap.ssid == ssid && g_ap.channel >= 1 &&
         g_ap.channel <= 14;
}

// How long an attempt that is not using the cached AP gets before it is started
// again from scratch. config::kWifiConnectTimeoutMs has how the budget divides.
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

// The lwIP client behind esp_netif. Null until the station has a netif, which
// is any time before WiFi.mode(WIFI_STA); the offered lease time and the BOUND
// state are the two things read through it, and neither has an accessor in
// arduino-esp32.
const struct dhcp* stationDhcp() {
  esp_netif_t* handle = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (handle == nullptr) return nullptr;
  struct netif* impl = static_cast<struct netif*>(esp_netif_get_netif_impl(handle));
  if (impl == nullptr) return nullptr;
  return netif_dhcp_data(impl);
}

// Seconds since an RTC counter reading, across however many deep sleeps. It has
// to be the RTC counter rather than esp_timer, which restarts on every wake and
// does not carry the sleep. The calibration is read now rather than stored: a
// lease is counted in hours and the slow clock's drift does not reach that far.
uint32_t secondsSince(uint64_t ticks) {
  const uint64_t now = rtc_time_get();
  if (now <= ticks) return 0;
  return static_cast<uint32_t>(
      rtc_time_slowclk_to_us(now - ticks, esp_clk_slowclk_cal_get()) / 1000000ULL);
}

// Whether there is an entry about this network at all, and how old it is.
bool leaseHolds(uint32_t ssid, uint32_t& ageS) {
  if (g_lease.magic != kCacheMagic || g_lease.ssid != ssid) return false;
  if (g_lease.ip == 0 || g_lease.seconds == 0) return false;

  ageS = secondsSince(g_lease.ticks);
  return true;
}

// Whether it is still worth installing.
//
// **Half the offered life is DHCP's own T1**, the point at which a client that
// had one running would be renewing rather than using the address. The firmware
// has no client running between questions, so the same boundary is where it
// stops reusing and asks properly -- which keeps the device inside the contract
// the server wrote rather than inside an interval invented here.
bool leaseIsYoung(uint32_t ageS) { return ageS < g_lease.seconds / 2; }

}  // namespace

void WifiLink::forgetAp() { g_ap.magic = 0; }

bool WifiLink::cachedLease(Lease& lease) {
  if (g_lease.magic != kCacheMagic || g_lease.ip == 0) return false;
  lease.ip = g_lease.ip;
  lease.gateway = g_lease.gateway;
  lease.mask = g_lease.mask;
  lease.dns = g_lease.dns;
  lease.seconds = g_lease.seconds;
  lease.ageS = secondsSince(g_lease.ticks);
  return true;
}

void WifiLink::forgetLease() { g_lease.magic = 0; }

bool WifiLink::begin(const char* ssid, const char* password, const char* dns) {
  if (ssid == nullptr || ssid[0] == '\0') {
    _state = State::Failed;
    _lastError = "no WiFi credentials in src/secrets.h";
    return false;
  }

  // An IPv6 address or 0.0.0.0 comes out of IPAddress as zero, and neither is
  // anything the stack could look a name up with.
  _customDns = 0;
  if (dns != nullptr && dns[0] != '\0') {
    IPAddress parsed;
    if (!parsed.fromString(dns) || static_cast<uint32_t>(parsed) == 0) {
      _state = State::Failed;
      _lastError = "the DNS server in src/secrets.h is not an IPv4 address";
      return false;
    }
    _customDns = static_cast<uint32_t>(parsed);
  }

  _ssid = ssid;
  _password = password;
  _state = State::Connecting;
  _lastError = "";
  _startUs = esp_timer_get_time();
  _settledMs = 0;
  _attemptEndsAtMs = 0;
  _retryAtMs = 0;
  _attempts = 0;
  _beginFailed = false;
  _usedCachedAp = false;
  _usedLease = false;
  _leaseSeconds = 0;
  _renewing = false;
  _renewStartUs = 0;
  _renewMs = 0;
  _channel = 0;
  _rssi = 0;
  _ipv4 = 0;
  _ip[0] = '\0';
  _usedCustomDns = false;
  _dns[0] = '\0';
  memset(_bssid, 0, sizeof(_bssid));

  // None of this belongs in NVS. The credentials are compiled in and the AP
  // lives in RTC memory, so persisting them would be a flash write on every
  // wake in exchange for nothing. Auto-reconnect is off for the same reason the
  // fallback exists: retries are this class's decision, taken against a budget,
  // not the library's taken against nothing.
  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(false);

  // Once per boot: WiFi.onEvent() appends, and a wake that registered twice
  // would keep two copies of a handler that is only ever written once.
  static bool listening = false;
  if (!listening) {
    listening = true;
    WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_CONNECTED);
    WiFi.onEvent(onWifiEvent, ARDUINO_EVENT_WIFI_STA_GOT_IP);
  }
  g_linkUs = 0;
  g_gotIpUs = 0;

  const uint32_t ssidHash = hashOf(ssid);

  // **The address goes in before the association, not after it.** WiFi.config()
  // needs the netif that WiFi.mode() has just created, and the station has to
  // already know it is not asking for an address by the time it associates --
  // otherwise the client starts, and then the whole 3.2 s is spent anyway.
  uint32_t ageS = 0;
  if (leaseHolds(ssidHash, ageS) && leaseIsYoung(ageS)) {
    _usedLease = WiFi.config(IPAddress(g_lease.ip), IPAddress(g_lease.gateway),
                             IPAddress(g_lease.mask), IPAddress(g_lease.dns));
    if (_usedLease) _leaseSeconds = g_lease.seconds;
  }

  startAttempt(apCacheHolds(ssidHash));
  return true;
}

WifiLink::State WifiLink::poll() {
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

  if (_renewing) return pollRenew();

  const wl_status_t status = WiFi.status();
  const uint32_t ms = elapsedMs();

  if (status == WL_CONNECTED) {
    _settledMs = ms;
    recordSuccess();
    installDns();
    _state = State::Online;
    return _state;
  }

  if (ms >= config::kWifiConnectTimeoutMs) {
    _settledMs = ms;
    _state = State::Failed;
    _lastError = "no association inside the budget";
    forgetAp();
    return _state;
  }

  // The attempt in flight has had its window. For one on the cached AP that
  // means the scan the firmware would have done without the cache starts
  // instead; for a scan it means starting over. Either way it is inside the same
  // budget. Only while there is no link -- see wifi_link.h.
  //
  // A begin() that refused to start anything is the one case that does not wait
  // for a window: there is nothing in flight to disturb.
  if (g_linkUs == 0 && (ms >= _attemptEndsAtMs || (_beginFailed && ms >= _retryAtMs))) {
    startAttempt(false);
  }

  return _state;
}

void WifiLink::renewAddress() {
  if (_state != State::Online || _renewing) return;

  // The entry goes first, so that a wake which ends badly from here -- no
  // address, a backend that is down anyway, a battery that runs out -- still
  // leaves the next one to ask properly rather than to repeat this.
  forgetLease();

  _renewing = true;
  _usedLease = false;
  _leaseSeconds = 0;
  _renewStartUs = esp_timer_get_time();
  _state = State::Connecting;

  // onlineMs() should mean the address this wake is running on, and the one it
  // means now is being thrown away.
  g_gotIpUs = 0;

  // An address of nothing is how arduino-esp32 is told to start the client:
  // NetworkInterface::config() stops it, zeroes the netif's addresses and
  // starts it again only when what it was handed is INADDR_NONE. The
  // association underneath is untouched -- what is being thrown away is the
  // address, not the link, and re-associating would cost the part that worked.
  //
  // A refusal here means no client was started, so there is nothing for
  // pollRenew() to wait for and waiting the whole budget would only spend it.
  if (!WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE)) {
    _renewing = false;
    _state = State::Failed;
    _lastError = "WiFi.config() would not start the DHCP client";
  }
}

WifiLink::State WifiLink::pollRenew() {
  const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - _renewStartUs) / 1000);

  // **WiFi.status() is history here and the address is not a witness either.**
  // The status is still the WL_CONNECTED the discarded address set, and it has
  // no event coming to replace it; and the address the server hands back is
  // usually the one just dropped, so "it changed" would wait for something that
  // is not going to happen. The client's own state is the only thing that
  // answers the question actually being asked.
  const struct dhcp* client = stationDhcp();
  if (client != nullptr && client->state == DHCP_STATE_BOUND &&
      static_cast<uint32_t>(WiFi.localIP()) != 0) {
    _renewMs = ms;
    _renewing = false;
    recordSuccess();
    installDns();
    _state = State::Online;
    return _state;
  }

  if (ms >= config::kWifiConnectTimeoutMs) {
    _renewMs = ms;
    _renewing = false;
    _state = State::Failed;
    _lastError = "no address after the lease was dropped";
  }
  return _state;
}

void WifiLink::end() {
  WiFi.disconnect(true, false, 0);  // stop the station, keep the config
  _state = State::Idle;
}

uint32_t WifiLink::elapsedMs() const {
  if (_state == State::Idle) return 0;
  if (_settledMs != 0) return _settledMs;
  return static_cast<uint32_t>((esp_timer_get_time() - _startUs) / 1000);
}

uint32_t WifiLink::linkMs() const {
  if (g_linkUs == 0 || _startUs == 0) return 0;
  return static_cast<uint32_t>((g_linkUs - _startUs) / 1000);
}

uint32_t WifiLink::onlineMs() const {
  if (g_gotIpUs == 0 || _startUs == 0) return 0;
  return static_cast<uint32_t>((g_gotIpUs - _startUs) / 1000);
}

void WifiLink::startAttempt(bool useCachedAp) {
  // Whatever the library still has in flight has to go first, or
  // esp_wifi_connect() refuses the new attempt -- and after the first failure
  // there is always something in flight, because WiFiSTA retries once on its
  // own whatever setAutoReconnect() was told.
  if (_attempts > 0) WiFi.disconnect(false, false, 0);

  const uint32_t ms = elapsedMs();
  _usedCachedAp = useCachedAp;
  ++_attempts;
  _attemptEndsAtMs = ms + (useCachedAp ? config::kWifiCachedApAttemptMs : kScanAttemptMs);
  _retryAtMs = ms + kRetryDelayMs;

  // WiFi.begin() reports a refusal as WL_CONNECT_FAILED, but on the way out it
  // also returns whatever the status already was -- and WL_CONNECT_FAILED is a
  // status a previous attempt can leave behind. Only a value that was not there
  // before the call is news, or a wrong password would have this restarting
  // every kRetryDelayMs for the whole budget.
  const wl_status_t before = WiFi.status();
  const wl_status_t started = useCachedAp
                                  ? WiFi.begin(_ssid, _password, g_ap.channel, g_ap.bssid)
                                  : WiFi.begin(_ssid, _password);
  _beginFailed = (started == WL_CONNECT_FAILED && before != WL_CONNECT_FAILED);
}

void WifiLink::recordSuccess() {
  WiFi.BSSID(_bssid);
  _channel = static_cast<uint8_t>(WiFi.channel());
  _rssi = WiFi.RSSI();

  const IPAddress ip = WiFi.localIP();
  _ipv4 = static_cast<uint32_t>(ip);
  snprintf(_ip, sizeof(_ip), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);

  // The AP that just worked, for the next wake. Written on every success rather
  // than only when the AP cache was empty, so a network that moved channel fixes
  // itself in one question instead of staying wrong.
  g_ap.magic = kCacheMagic;
  g_ap.ssid = hashOf(_ssid);
  memcpy(g_ap.bssid, _bssid, kBssidBytes);
  g_ap.channel = _channel;

  // **Only a wake that actually held the exchange has a lease worth keeping.**
  // On a wake that installed the cached address there is nothing new to write:
  // the address is the one already in the entry, the offered life is whatever
  // the server said when it granted it, and rewriting the timestamp would make
  // an entry that can never age out -- it would be a day old and claim to be
  // fresh on every question.
  if (_usedLease) return;

  // The life of the lease is the server's own number, read from the client that
  // has just finished. Without it there is nothing to age the entry against, so
  // the entry is not written at all and the next wake asks properly: a lease
  // reused on a guess about its life would be exactly the claim on someone
  // else's address the lease cache is built not to make.
  const struct dhcp* client = stationDhcp();
  _leaseSeconds = client != nullptr ? client->offered_t0_lease : 0;
  if (_ipv4 == 0 || _leaseSeconds == 0) return;

  g_lease.magic = kCacheMagic;
  g_lease.ssid = g_ap.ssid;
  g_lease.ip = _ipv4;
  g_lease.gateway = static_cast<uint32_t>(WiFi.gatewayIP());
  g_lease.mask = static_cast<uint32_t>(WiFi.subnetMask());
  g_lease.dns = static_cast<uint32_t>(WiFi.dnsIP(0));
  g_lease.seconds = _leaseSeconds;
  g_lease.ticks = rtc_time_get();
}

void WifiLink::installDns() {
  // In place of the network's, not ahead of it: setDNS() clears the backup slot
  // as well, so the server read back below is the only one asked.
  _usedCustomDns = _customDns != 0 && WiFi.setDNS(IPAddress(_customDns));

  const IPAddress dns = WiFi.dnsIP(0);
  snprintf(_dns, sizeof(_dns), "%u.%u.%u.%u", dns[0], dns[1], dns[2], dns[3]);
}
