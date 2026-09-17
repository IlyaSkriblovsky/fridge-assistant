// S6 -- WiFi. A temporary driver, the way S1's to S5's were:
// docs/implementation.md asks for connect times over Serial1, cold and cached,
// including the first boot after the cache was written and a run with the
// access point moved to another channel. S8 replaces all of this with the
// orchestrator.
//
// One association per wake, which is the only shape that can measure the thing
// this step is about: the BSSID and channel cache lives in RTC memory, so it
// only means anything across a deep sleep. A single boot that connected ten
// times would measure ten warm reconnects and nothing else.
//
// So the driver sleeps between cycles, and every wake is one row of a table it
// keeps in RTC memory and reprints as it grows:
//
//   1                  the cold connect -- RTC memory is cleared by the reset
//                      that flashing or opening the monitor causes, so the
//                      first row of every session has no cache behind it
//   2 .. kAutoCycles   cached connects, on the timer, unattended
//   the rest           one per press of the AI button, for the scenarios that
//                      need a human: move the access point to another channel
//                      between two presses and watch the cached attempt fail,
//                      the fallback pick the new channel up, and the press
//                      after that be fast again
//
// **The board has no off switch**, so nothing here repeats by itself past the
// timer cycles: after them the sleep is armed on the button alone and the board
// sits asleep until it is pressed. The same reasoning as S1's listening test
// and S5's walkthrough.
//
// Every cycle ends in a chirp -- ready for an association, error for NO WIFI --
// so a run is legible on battery, where there is no console at all.

#include <Arduino.h>
#include <esp_timer.h>

#include "config.h"
#include "secrets.h"
#include "sticky_button.h"
#include "sticky_buzzer.h"
#include "sticky_power.h"
#include "sticky_wifi.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// Unattended wakes before the driver hands the pace over to the button. Enough
// cached connects to see a spread; a single sample is not a measurement.
constexpr uint32_t kAutoCycles = 6;

// Long enough for the association to be genuinely cold at the radio rather than
// a re-association the AP was still expecting, short enough to sit through.
constexpr uint64_t kAutoSleepUs = 5000000;

// How often the association is looked at. The orchestrator will poll at
// whatever rate its own loop turns; this is also the resolution of linkMs(),
// which is why it is small.
constexpr uint32_t kPollMs = 10;

// A press held through the report would wake the board again the moment it goes
// to sleep, since ext1 wakes on the level rather than on an edge. So the driver
// waits the press out first, up to this long.
constexpr uint32_t kReleaseWaitMs = 30000;

// The log lives in RTC memory, which survives deep sleep but not a power cycle
// or a reset -- so a session is exactly what this table holds, and the first
// row of one is always the cold connect.
constexpr uint32_t kLogRows = 24;
constexpr uint32_t kMagic = 0x56C0FFEE;

struct Row {
  uint16_t elapsedMs;  // begin() to an IP, or to giving up
  uint16_t linkMs;     // begin() to the link coming up, 0 if it was not caught
  uint16_t bootMs;     // the top of setup() to the same moment as elapsedMs
  uint32_t ipv4;
  uint8_t bssid[StickyWifi::kBssidBytes];
  uint8_t channel;
  uint8_t cachedChannel;  // what the cache said before the attempt, 0 for none
  int8_t rssi;
  uint8_t attempts;
  uint8_t wakeCause;  // esp_sleep_wakeup_cause_t, as recorded on that wake
  bool online;
  bool hadCache;
  bool usedCache;
};

RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_cycles;
RTC_DATA_ATTR Row g_log[kLogRows];

// Header and rows go through the same format string, so the columns line up.
// Every field is a string: anything that does not apply prints a dash.
constexpr const char* kRowFormat = "%3s  %-4s %-8s %8s %8s %8s %4s %6s  %-17s %s\n";

StickyButton button;
StickyWifi wifi;

void formatBssid(char* out, size_t size, const uint8_t* bssid) {
  snprintf(out, size, "%02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2], bssid[3],
           bssid[4], bssid[5]);
}

// pwr / tmr / btn, which is all this column has to say: the first row of a
// session is the cold one, and the rest are the two ways a cycle can be paced.
const char* wakeShortName(uint8_t cause) {
  switch (static_cast<esp_sleep_wakeup_cause_t>(cause)) {
    case ESP_SLEEP_WAKEUP_TIMER: return "tmr";
    case ESP_SLEEP_WAKEUP_EXT1: return "btn";
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "pwr";
    default: return "?";
  }
}

// What the attempt had to do, which is the whole point of the table: a cached
// connect and a fallback are the two numbers this step exists to compare.
const char* pathName(const Row& row) {
  if (!row.hadCache) return "scan";
  return row.usedCache ? "cached" : "fallback";
}

void printTable() {
  Serial1.println();
  Serial1.printf(kRowFormat, "#", "wake", "path", "link", "online", "boot", "ch", "rssi", "bssid",
                 "ip");

  const uint32_t rows = g_cycles < kLogRows ? g_cycles : kLogRows;
  for (uint32_t i = 0; i < rows; ++i) {
    const Row& row = g_log[i];

    char link[12] = "--";
    char online[12] = "FAILED";
    char boot[12] = "--";
    char channel[8] = "--";
    char rssi[8] = "--";
    char bssid[18] = "--";
    char ip[16] = "--";
    char index[8];

    snprintf(index, sizeof(index), "%lu", static_cast<unsigned long>(i + 1));
    if (row.linkMs != 0) snprintf(link, sizeof(link), "%u ms", row.linkMs);
    snprintf(boot, sizeof(boot), "%u ms", row.bootMs);

    if (row.online) {
      snprintf(online, sizeof(online), "%u ms", row.elapsedMs);
      snprintf(channel, sizeof(channel), "%u", row.channel);
      snprintf(rssi, sizeof(rssi), "%d", row.rssi);
      formatBssid(bssid, sizeof(bssid), row.bssid);
      snprintf(ip, sizeof(ip), "%u.%u.%u.%u", static_cast<unsigned>(row.ipv4 & 0xFF),
               static_cast<unsigned>((row.ipv4 >> 8) & 0xFF),
               static_cast<unsigned>((row.ipv4 >> 16) & 0xFF),
               static_cast<unsigned>((row.ipv4 >> 24) & 0xFF));
    } else {
      snprintf(online, sizeof(online), "%u ms!", row.elapsedMs);
    }

    Serial1.printf(kRowFormat, index, wakeShortName(row.wakeCause), pathName(row), link, online,
                   boot, channel, rssi, bssid, ip);
  }
  Serial1.println();
}

void record(const Row& row) {
  if (g_magic != kMagic) {
    g_magic = kMagic;
    g_cycles = 0;
  }
  if (g_cycles < kLogRows) g_log[g_cycles] = row;
  ++g_cycles;
}

// A press held through the report is still down when the sleep is armed, and
// ext1 wakes on the level: the board would wake again immediately and the next
// cycle would measure a press nobody made.
void waitForRelease() {
  const int64_t deadlineUs = esp_timer_get_time() + kReleaseWaitMs * 1000LL;
  if (!button.isDown()) return;

  Serial1.println("  waiting for the button to come back up");
  while (button.isDown() && esp_timer_get_time() < deadlineUs) delay(5);
}

[[noreturn]] void sleepUntilNextCycle() {
  wifi.end();
  waitForRelease();

  const bool autoCycle = g_cycles < kAutoCycles;
  if (autoCycle) {
    Serial1.printf("  sleeping %lu s for cycle %lu\n",
                   static_cast<unsigned long>(kAutoSleepUs / 1000000),
                   static_cast<unsigned long>(g_cycles + 1));
  } else {
    Serial1.println("  press the AI button for another cycle");
  }
  Serial1.flush();

  stickyPower::deepSleep(autoCycle ? kAutoSleepUs : 0);
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(tEntry);  // takes GPIO4 back from the RTC pad and pulls it up

  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  const uint8_t wakeCause = static_cast<uint8_t>(esp_sleep_get_wakeup_cause());
  const uint32_t cycle = (g_magic == kMagic ? g_cycles : 0) + 1;

  Serial1.println();
  Serial1.printf("S6 wifi driver -- cycle %lu, wake %s, reset %s\n",
                 static_cast<unsigned long>(cycle), stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());

  Row row = {};
  row.wakeCause = wakeCause;

  uint8_t cachedBssid[StickyWifi::kBssidBytes];
  uint8_t cachedChannel = 0;
  if (StickyWifi::cachedAp(cachedBssid, cachedChannel)) {
    char text[18];
    formatBssid(text, sizeof(text), cachedBssid);
    Serial1.printf("  cache: %s on channel %u\n", text, cachedChannel);
    row.cachedChannel = cachedChannel;
  } else {
    Serial1.println("  cache: empty -- this connect is cold");
  }

  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword)) {
    Serial1.printf("FAILED: %s\n", wifi.lastError());
    Serial1.flush();
    stickyBuzzer::error();
    stickyPower::deepSleep();
  }

  Serial1.printf("  connecting to \"%s\"...\n", secrets::kWifiSsid);
  while (wifi.poll() == StickyWifi::State::Connecting) delay(kPollMs);

  const int64_t settledUs = esp_timer_get_time();
  row.online = wifi.online();
  row.elapsedMs = static_cast<uint16_t>(wifi.elapsedMs());
  row.linkMs = static_cast<uint16_t>(wifi.linkMs());
  row.bootMs = static_cast<uint16_t>((settledUs - tEntry) / 1000);
  row.hadCache = wifi.hadCache();
  row.usedCache = wifi.usedCache();
  row.attempts = static_cast<uint8_t>(wifi.attempts());

  if (wifi.online()) {
    char text[18];
    formatBssid(text, sizeof(text), wifi.bssid());
    memcpy(row.bssid, wifi.bssid(), StickyWifi::kBssidBytes);
    row.channel = wifi.channel();
    row.rssi = wifi.rssi();
    row.ipv4 = wifi.ipv4();

    Serial1.printf("  online in %lu ms (%lu ms from boot): %s, ch %u, %d dBm, bssid %s\n",
                   static_cast<unsigned long>(row.elapsedMs), static_cast<unsigned long>(row.bootMs),
                   wifi.ip(), static_cast<unsigned>(row.channel), static_cast<int>(row.rssi), text);
    Serial1.printf("         %s, %lu attempt%s\n", pathName(row),
                   static_cast<unsigned long>(row.attempts), row.attempts == 1 ? "" : "s");
    if (row.linkMs != 0) {
      // The link is the association and the authentication; everything after it
      // is DHCP, which is the half a static address would remove.
      Serial1.printf("         link up at %lu ms, so DHCP was %lu ms of it\n",
                     static_cast<unsigned long>(row.linkMs),
                     static_cast<unsigned long>(row.elapsedMs - row.linkMs));
    } else {
      Serial1.println("         the link coming up fell between two polls");
    }
  } else {
    Serial1.printf("  NO WIFI after %lu ms: %s (%lu attempts)\n",
                   static_cast<unsigned long>(row.elapsedMs), wifi.lastError(),
                   static_cast<unsigned long>(row.attempts));
  }

  record(row);
  printTable();

  // The sound the flow makes at this point of a question, so a run is legible
  // without a console -- which is the only way it is legible on battery.
  if (row.online) {
    stickyBuzzer::ready();
  } else {
    stickyBuzzer::error();
  }

  sleepUntilNextCycle();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
