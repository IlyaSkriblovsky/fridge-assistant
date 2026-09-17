// E6 -- what the DHCP exchange is made of, and what a cached lease removes.
// See docs/experiments.md. Build and flash with:
//
//     ~/.platformio/penv/bin/pio run -e exp_e6
//     ~/.platformio/penv/bin/pio run -e exp_e6 -t upload --upload-port <port>
//
// This replaces main.cpp in its own environment; nothing here is part of the
// firmware. S6 measured a connect on the home network and found the association
// is nothing and the address is everything -- the link comes up in 87 ms and
// the IP arrives 3.15 s later, on every wake, too consistently to be a busy
// server. This rig asks where those seconds go and whether they can be skipped.
//
// Two modes, alternating wake by wake so both see the same network within a
// minute of each other:
//
//   dhcp    what the firmware does today. The lwIP client's own state machine
//           is sampled every 2 ms through netif_dhcp_data(), so a row carries
//           the whole timeline: SELECTING (a DISCOVER is out), REQUESTING (an
//           OFFER came back), CHECKING (the ARP check that this build's
//           lwipopts.h says takes 1-2 s), BOUND. The retry count and the lease
//           the server offered come from the same struct.
//   cached  the lease the last dhcp wake wrote to RTC memory, installed with
//           WiFi.config() before the association, which stops the client from
//           running at all.
//
// **An address nobody used is not an address**, so neither mode is believed on
// its own: every wake ends with a TCP connect, the backend first and the
// gateway second. A refusal proves as much as an acceptance -- both mean the
// network answered this address -- and only a timeout means it did not. The row
// reports which target answered and how long it took.
//
// Reading the lwIP struct from this task while the TCP/IP task owns it is a
// race, and an acceptable one for a rig: the two fields sampled are a byte
// each, nothing is written back, and a torn read shows up as a state that does
// not fit the sequence rather than as a wrong number.
//
// The pacing is E1's: kAutoCycles wakes on the timer, unattended, and then the
// button, because the board has no off switch.

#include <Arduino.h>
#include <WiFi.h>
#include <esp_netif.h>
#include <esp_netif_net_stack.h>
#include <esp_timer.h>
#include <lwip/dhcp.h>
#include <lwip/netif.h>
#include <lwip/prot/dhcp.h>  // the DHCP_STATE_* names; lwip/dhcp.h has only the struct

#include "config.h"
#include "secrets.h"

#include "sticky/buzzer.h"
#include "sticky/power.h"

#include "wifi_link.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// Six wakes of each mode before the rig hands the pace over to the button.
constexpr uint32_t kAutoCycles = 12;
constexpr uint64_t kAutoSleepUs = 5000000;

// Also the resolution of the timeline, which is why it is this small.
constexpr uint32_t kPollMs = 2;

// Long enough that a timeout means the network really did not answer, short
// enough that a wake spent waiting for one is still a short wake.
constexpr uint32_t kProbeTimeoutMs = 1500;

// Every sixth wake installs a lease that cannot work -- the right address on
// the wrong network -- because a cached lease is only worth having if the
// firmware can notice it has gone stale and recover inside the same question.
// This is the number that says how much that costs.
constexpr uint32_t kPoisonEvery = 4;
constexpr uint32_t kRecoverTimeoutMs = 15000;

constexpr uint32_t kLogRows = 24;
constexpr uint32_t kMaxSamples = 12;
constexpr uint32_t kMagic = 0xE6C0FFEE;

enum Target : uint8_t { kBackend = 0, kGateway = 1, kNothing = 2 };

struct Lease {
  uint32_t ip;
  uint32_t gw;
  uint32_t mask;
  uint32_t dns;
  uint32_t seconds;  // what the server offered, from the lwIP struct
  bool valid;
};

struct Row {
  uint8_t mode;  // 0 dhcp, 1 cached, 2 cached but deliberately stale
  uint16_t linkMs;
  uint16_t ipMs;
  uint16_t probeMs;   // both attempts together, not just the one that answered
  uint16_t recoverMs;  // 0 unless a stale lease had to be thrown away
  uint16_t bootMs;  // the top of setup() to the probe being over
  uint8_t tries;
  uint8_t target;
  uint32_t leaseS;
  bool online;
  bool connected;  // the probe completed a handshake
  bool answered;   // ... or was refused, which proves the address just as well
};

RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_cycles;
RTC_DATA_ATTR Lease g_lease;
RTC_DATA_ATTR Row g_log[kLogRows];

struct Sample {
  uint8_t state;
  uint16_t ms;
};

Sample g_samples[kMaxSamples];
uint8_t g_sampleCount = 0;
uint8_t g_lastState = 0xFF;
uint8_t g_tries = 0;
uint32_t g_leaseSeconds = 0;

WifiLink wifi;

constexpr const char* kRowFormat = "%3s  %-6s %7s %8s %8s %8s %5s  %-8s %s\n";

const char* dhcpStateName(uint8_t state) {
  switch (state) {
    case DHCP_STATE_OFF: return "OFF";
    case DHCP_STATE_REQUESTING: return "REQUESTING";
    case DHCP_STATE_INIT: return "INIT";
    case DHCP_STATE_REBOOTING: return "REBOOTING";
    case DHCP_STATE_REBINDING: return "REBINDING";
    case DHCP_STATE_RENEWING: return "RENEWING";
    case DHCP_STATE_SELECTING: return "SELECTING";
    case DHCP_STATE_INFORMING: return "INFORMING";
    case DHCP_STATE_CHECKING: return "CHECKING";
    case DHCP_STATE_PERMANENT: return "PERMANENT";
    case DHCP_STATE_BOUND: return "BOUND";
    case DHCP_STATE_RELEASING: return "RELEASING";
    case DHCP_STATE_BACKING_OFF: return "BACKING_OFF";
    default: return "?";
  }
}

// What each state means in packets, which is the whole point of the timeline.
const char* dhcpStateNote(uint8_t state) {
  switch (state) {
    case DHCP_STATE_INIT: return "the client exists";
    case DHCP_STATE_SELECTING: return "DISCOVER is out";
    case DHCP_STATE_REQUESTING: return "an OFFER came back, REQUEST is out";
    case DHCP_STATE_REBOOTING: return "asking for the previous address";
    case DHCP_STATE_CHECKING: return "the ACK is in, ARP-checking the address";
    case DHCP_STATE_BOUND: return "the address is ours";
    case DHCP_STATE_BACKING_OFF: return "nobody answered; waiting to try again";
    default: return "";
  }
}

// The lwIP client behind the esp_netif wrapper. Null until the station has
// started and the client has been created, which is most of the first 100 ms.
struct dhcp* stationDhcp() {
  esp_netif_t* handle = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  if (handle == nullptr) return nullptr;
  struct netif* impl = static_cast<struct netif*>(esp_netif_get_netif_impl(handle));
  if (impl == nullptr) return nullptr;
  return netif_dhcp_data(impl);
}

void sampleDhcp(uint32_t ms) {
  const struct dhcp* client = stationDhcp();
  if (client == nullptr) return;

  g_tries = client->tries;
  if (client->offered_t0_lease != 0) g_leaseSeconds = client->offered_t0_lease;

  if (client->state == g_lastState) return;
  g_lastState = client->state;
  if (g_sampleCount < kMaxSamples) {
    g_samples[g_sampleCount].state = client->state;
    g_samples[g_sampleCount].ms = static_cast<uint16_t>(ms);
    ++g_sampleCount;
  }
}

void printTimeline() {
  if (g_sampleCount == 0) {
    Serial1.println("  dhcp: the client never appeared -- nothing was asked of it");
    return;
  }

  Serial1.println("  dhcp timeline, from begin():");
  for (uint8_t i = 0; i < g_sampleCount; ++i) {
    Serial1.printf("     %6u ms  %-12s %s\n", g_samples[i].ms, dhcpStateName(g_samples[i].state),
                   dhcpStateNote(g_samples[i].state));
  }
  Serial1.printf("     %u retr%s, lease %lu s\n", g_tries, g_tries == 1 ? "y" : "ies",
                 static_cast<unsigned long>(g_leaseSeconds));
}

struct Probe {
  bool connected;
  bool answered;
  uint32_t ms;
  uint8_t target;
};

// One TCP connect, to prove the address is not just installed but usable. The
// backend is the target the firmware actually needs; the gateway is the one
// that is always there. A connection refused inside the timeout is as good an
// answer as a connection accepted -- something on the network replied to this
// address -- so the two are reported apart.
Probe probeNetwork(const IPAddress& gateway) {
  // Without an address of our own nothing can be proved: lwIP fails a connect
  // in microseconds because it has nowhere to send from, and that fast failure
  // would otherwise read exactly like a refusal from a working network.
  if (static_cast<uint32_t>(WiFi.localIP()) == 0) {
    Serial1.println("  probe: no address at all, so there is nothing to probe with");
    return {false, false, 0, kNothing};
  }

  const int64_t startUs = esp_timer_get_time();
  NetworkClient client;

  // The gateway goes first because it is always there. The backend is the
  // target the firmware actually needs, but it is a host that can be switched
  // off, and a probe that times out on it would add its own seconds to every
  // row.
  const bool gatewayOk = client.connect(gateway, 80, kProbeTimeoutMs) == 1;
  const uint32_t gatewayMs = static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
  client.stop();
  Serial1.printf("  probe: the gateway %s after %lu ms\n",
                 gatewayOk ? "accepted" : (gatewayMs + 50 < kProbeTimeoutMs ? "refused" : "timed out"),
                 static_cast<unsigned long>(gatewayMs));
  if (gatewayOk || gatewayMs + 50 < kProbeTimeoutMs) return {gatewayOk, true, gatewayMs, kGateway};

  char host[48] = {0};
  unsigned int port = 80;
  if (sscanf(config::kBackendBaseUrl, "http://%47[^:/]:%u", host, &port) < 1) host[0] = '\0';
  if (host[0] == '\0') {
    return {false, false, gatewayMs, kNothing};
  }

  const int64_t backendUs = esp_timer_get_time();
  const bool ok = client.connect(host, static_cast<uint16_t>(port), kProbeTimeoutMs) == 1;
  const uint32_t backendMs = static_cast<uint32_t>((esp_timer_get_time() - backendUs) / 1000);
  const uint32_t totalMs = static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
  client.stop();
  Serial1.printf("  probe: the backend %s after %lu ms\n",
                 ok ? "accepted" : (backendMs + 50 < kProbeTimeoutMs ? "refused" : "timed out"),
                 static_cast<unsigned long>(backendMs));

  if (ok || backendMs + 50 < kProbeTimeoutMs) return {ok, true, totalMs, kBackend};
  return {false, false, totalMs, kNothing};
}

// Throws the installed address away and asks for one properly, which is what
// the firmware would have to do the first time a question found its cached
// lease no longer belonged to it.
uint32_t recoverWithDhcp(const IPAddress& stale) {
  const int64_t startUs = esp_timer_get_time();
  g_sampleCount = 0;
  g_lastState = 0xFF;

  WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE);

  // Waiting on WiFi.status() is what the first version did, and it returned in
  // a millisecond with no address at all: the status was still the
  // WL_CONNECTED the static address had set, and nothing had happened yet to
  // replace it. The address itself is the only thing here that cannot be
  // history -- so the wait is for one that is real and is not the one just
  // thrown away.
  for (;;) {
    const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
    const IPAddress now = WiFi.localIP();
    if (static_cast<uint32_t>(now) != 0 && now != stale && WiFi.status() == WL_CONNECTED) return ms;
    if (ms >= kRecoverTimeoutMs) return ms;
    sampleDhcp(ms);
    delay(kPollMs);
  }
}

const char* targetName(uint8_t target) {
  switch (target) {
    case kBackend: return "backend";
    case kGateway: return "gateway";
    default: return "--";
  }
}

void printTable() {
  Serial1.println();
  Serial1.printf(kRowFormat, "#", "mode", "link", "ip", "probe", "boot", "retr", "answered",
                 "recovery");

  const uint32_t rows = g_cycles < kLogRows ? g_cycles : kLogRows;
  for (uint32_t i = 0; i < rows; ++i) {
    const Row& row = g_log[i];

    char index[8], link[12] = "--", ip[12] = "FAILED", probe[12] = "--", boot[12], retries[8],
        lease[12] = "--";
    char answered[10];

    snprintf(index, sizeof(index), "%lu", static_cast<unsigned long>(i + 1));
    // A cached lease reaches WL_CONNECTED without ever passing through
    // WL_IDLE_STATUS: there is no DHCP in between for the poll to catch it in.
    if (row.linkMs == 0) {
      snprintf(link, sizeof(link), "--");
    } else {
      snprintf(link, sizeof(link), "%u ms", row.linkMs);
    }
    snprintf(boot, sizeof(boot), "%u ms", row.bootMs);
    snprintf(retries, sizeof(retries), "%u", row.tries);
    if (row.recoverMs != 0) {
      snprintf(lease, sizeof(lease), "%u ms", row.recoverMs);
    } else {
      snprintf(lease, sizeof(lease), "--");
    }

    if (row.online) snprintf(ip, sizeof(ip), "%u ms", row.ipMs);
    if (row.online) snprintf(probe, sizeof(probe), "%u ms", row.probeMs);
    snprintf(answered, sizeof(answered), "%s%s", row.answered ? "" : "no ",
             row.answered ? targetName(row.target) : "");

    const char* mode = row.mode == 0 ? "dhcp" : row.mode == 1 ? "cached" : "stale";
    Serial1.printf(kRowFormat, index, mode, link, ip, probe, boot, retries, answered, lease);
  }
  Serial1.println();
}

void record(const Row& row) {
  if (g_magic != kMagic) {
    g_magic = kMagic;
    g_cycles = 0;
    g_lease.valid = false;
  }
  if (g_cycles < kLogRows) g_log[g_cycles] = row;
  ++g_cycles;
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  stickyPower::holdLatch();
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  const uint32_t cycle = (g_magic == kMagic ? g_cycles : 0) + 1;
  const bool haveLease = (g_magic == kMagic) && g_lease.valid;

  // Odd wakes take the address the normal way, even wakes reuse the last one --
  // and the first wake of a session has nothing to reuse, so it is a dhcp wake
  // whatever its number says.
  const bool cached = (cycle % 2 == 0) && haveLease;
  const bool poisoned = cached && (cycle % kPoisonEvery == 0);

  Serial1.println();
  Serial1.printf("E6 dhcp rig -- cycle %lu, %s, wake %s\n", static_cast<unsigned long>(cycle),
                 poisoned ? "a lease from the wrong network" : cached ? "cached lease" : "dhcp",
                 stickyPower::wakeupCauseName());

  Row row = {};
  row.mode = poisoned ? 2 : cached ? 1 : 0;

  // The netif has to exist before a static address will take, and WiFi.mode()
  // is what creates it. WifiLink::begin() calls it again, which is free.
  //
  // **persistent(false) goes first**, and the first run of this rig is how that
  // was learned: with it after the mode, every wake spent over a second more
  // before the association even started. Arduino's default storage is FLASH, so
  // a mode set while that is still true goes through NVS. WifiLink does it in
  // this order already; the rig has to as well, or it measures a different
  // firmware than the one it is reporting on.
  const int64_t tSerial = esp_timer_get_time();
  WiFi.persistent(false);
  const int64_t tPersistent = esp_timer_get_time();
  WiFi.mode(WIFI_STA);
  const int64_t tMode = esp_timer_get_time();

  if (cached) {
    // The stale case is an address that is perfectly valid somewhere else: the
    // subnet the device woke up on is not the one the lease came from, so the
    // gateway will never answer it.
    const IPAddress ip = poisoned ? IPAddress(10, 42, 0, 56) : IPAddress(g_lease.ip);
    const IPAddress gw = poisoned ? IPAddress(10, 42, 0, 254) : IPAddress(g_lease.gw);
    const IPAddress mask = poisoned ? IPAddress(255, 255, 255, 0) : IPAddress(g_lease.mask);
    const IPAddress dns = poisoned ? IPAddress(10, 42, 0, 254) : IPAddress(g_lease.dns);
    Serial1.printf("  installing %s, gw %s, dns %s -- no client will run\n", ip.toString().c_str(),
                   gw.toString().c_str(), dns.toString().c_str());
    if (!WiFi.config(ip, gw, mask, dns)) Serial1.println("  WiFi.config() refused the lease");
  }
  const int64_t tConfig = esp_timer_get_time();

  Serial1.printf("  before begin(): %lu ms boot and console, %lu ms persistent, %lu ms mode, %lu ms config\n",
                 static_cast<unsigned long>((tSerial - tEntry) / 1000),
                 static_cast<unsigned long>((tPersistent - tSerial) / 1000),
                 static_cast<unsigned long>((tMode - tPersistent) / 1000),
                 static_cast<unsigned long>((tConfig - tMode) / 1000));

  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword)) {
    Serial1.printf("FAILED: %s\n", wifi.lastError());
    Serial1.flush();
    stickyBuzzer::error();
    stickyPower::deepSleep();
  }

  while (wifi.poll() == WifiLink::State::Connecting) {
    sampleDhcp(wifi.elapsedMs());
    delay(kPollMs);
  }
  sampleDhcp(wifi.elapsedMs());

  row.online = wifi.online();
  row.linkMs = static_cast<uint16_t>(wifi.linkMs());
  row.ipMs = static_cast<uint16_t>(wifi.elapsedMs());
  row.tries = g_tries;
  row.leaseS = cached ? g_lease.seconds : g_leaseSeconds;

  if (row.online) {
    Serial1.printf("  %s in %lu ms: link at %lu ms, address %lu ms later\n",
                   cached ? "installed" : "leased", static_cast<unsigned long>(row.ipMs),
                   static_cast<unsigned long>(row.linkMs),
                   static_cast<unsigned long>(row.ipMs - row.linkMs));

    const Probe probe = probeNetwork(WiFi.gatewayIP());
    row.probeMs = static_cast<uint16_t>(probe.ms);
    row.connected = probe.connected;
    row.answered = probe.answered;
    row.target = probe.target;

    if (probe.answered) {
      Serial1.printf("  the address works: the %s answered in %lu ms of probing\n",
                     targetName(probe.target), static_cast<unsigned long>(probe.ms));
    } else {
      Serial1.printf("  nothing answered in %lu ms of probing -- this address is not usable\n",
                     static_cast<unsigned long>(probe.ms));

      // The whole point of the stale cycles: notice, throw it away, and ask
      // properly, all inside the question that found the problem.
      const uint32_t recovered = recoverWithDhcp(WiFi.localIP());
      row.recoverMs = static_cast<uint16_t>(recovered);
      const bool ok = WiFi.status() == WL_CONNECTED;
      Serial1.printf("  recovery: DHCP %s after %lu ms, now %s\n",
                     ok ? "answered" : "gave up", static_cast<unsigned long>(recovered),
                     WiFi.localIP().toString().c_str());
      if (ok) {
        const Probe again = probeNetwork(WiFi.gatewayIP());
        row.answered = again.answered;
        row.connected = again.connected;
        row.target = again.target;
        row.probeMs = static_cast<uint16_t>(probe.ms + again.ms);
      }
      printTimeline();
    }

    // Only a wake that actually held the exchange has a lease worth keeping.
    if (!cached || poisoned) {
      g_lease.ip = WiFi.localIP();
      g_lease.gw = WiFi.gatewayIP();
      g_lease.mask = WiFi.subnetMask();
      g_lease.dns = WiFi.dnsIP(0);
      g_lease.seconds = g_leaseSeconds;
      g_lease.valid = probe.answered;
    }
  } else {
    Serial1.printf("  no address after %lu ms: %s\n", static_cast<unsigned long>(row.ipMs),
                   wifi.lastError());
  }

  row.bootMs = static_cast<uint16_t>((esp_timer_get_time() - tEntry) / 1000);

  if (!cached) printTimeline();
  record(row);
  printTable();

  if (row.online && row.answered) {
    stickyBuzzer::ready();
  } else {
    stickyBuzzer::error();
  }

  wifi.end();

  const bool autoCycle = g_cycles < kAutoCycles;
  Serial1.println(autoCycle ? "  sleeping" : "  press the AI button for another cycle");
  Serial1.flush();
  stickyPower::deepSleep(autoCycle ? kAutoSleepUs : 0);
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
