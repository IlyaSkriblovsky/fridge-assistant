// S2 -- the AI button on the device. A temporary driver, the way S1's listening
// test was: docs/implementation.md asks for the hold length over Serial1 across
// a few dozen presses, and S8 replaces all of this with the orchestrator.
//
// One press per wake, which is the path the firmware will take: the board
// sleeps, the press wakes it, and the press is already down when setup() runs.
// Each wake prints one row and goes back to sleep, so a few dozen presses is a
// few dozen rows and the board is never left running -- it has no off switch.
//
// The chirps are the ones from the flow. Ready when the press has been
// registered (in the firmware it will mean the microphone is live), answer when
// the press was long enough to count. A tap gets the ready chirp and nothing
// after it, which is [D7](docs/deferred.md) made audible.
//
// Nothing polls the button until the UART is up and the ready chirp is over, so
// a press shorter than those reads as the 161 ms they take together rather than
// its own length. It costs nothing here: the minimum hold is 300 ms and
// everything at stake is above it. In the firmware the floor goes away on its
// own -- S4 moves the poll into the capture task, which does not stop for a
// chirp.

#include <Arduino.h>
#include <esp_private/esp_clk.h>
#include <esp_timer.h>
#include <soc/rtc.h>

#include "config.h"
#include "sticky_button.h"
#include "sticky_buzzer.h"
#include "sticky_power.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// A line per second while the button is held, so a long press shows life
// instead of silence.
constexpr uint32_t kProgressMs = 1000;

// The press count survives the sleeps but not a power-on or the reset that
// opening the serial monitor causes -- which is the session boundary wanted
// here, since the monitor is what the rows are being read on.
constexpr uint32_t kMagic = 0x5B2B0770;
RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_presses;

// What tells a bounce from a second press: a row that lands a boot's worth of
// time after the previous sleep is the release re-triggering ext1, while a
// human pressing again is hundreds of milliseconds at the very least. The floor
// is the boot itself, ~60 ms, which is inside this interval.
//
// Off the RTC counter, which is the only clock here that runs through deep
// sleep. esp_timer restarts on every wake, so a reading taken before the sleep
// and one taken after it are not on the same scale -- measured with this driver,
// and it corrects what docs/experiments.md used to say in E1.
RTC_DATA_ATTR uint64_t g_sleptAtTicks;

// Header and rows go through the same format string so the columns line up.
constexpr const char* kRowFormat = "%4s  %-5s %-9s %5s %7s %8s  %s\n";

StickyButton button;

void printBanner() {
  Serial1.println();
  Serial1.println("S2 button driver -- one press per wake, then back to sleep");
  Serial1.printf("  a press under %lu ms is a tap; release needs %lu ms of stable high\n",
                 static_cast<unsigned long>(config::kButtonMinHoldMs),
                 static_cast<unsigned long>(config::kButtonDebounceMs));
  Serial1.println("  held is short by the boot, ~60 ms, which cannot be measured here");
  Serial1.println("  slept is sleep entry to this wake, off the RTC counter: a row that");
  Serial1.println("  is only a boot behind the last one is a bounce, not a second press");
  Serial1.println();
  Serial1.printf(kRowFormat, "#", "wake", "reset", "down", "held", "slept", "verdict");
}

// Microseconds to a millisecond cell, with a dash where the number does not
// exist -- the first row of a session has no sleep behind it.
const char* msCell(char* buf, size_t size, int64_t us, bool valid) {
  if (valid) {
    snprintf(buf, size, "%lld", us / 1000);
  } else {
    snprintf(buf, size, "--");
  }
  return buf;
}

void printRow(uint32_t index, bool downAtStart, uint32_t heldMs, int64_t sleptUs,
              bool sleptValid, bool tap) {
  char cIndex[8], cHeld[10], cSlept[12];
  snprintf(cIndex, sizeof(cIndex), "%lu", static_cast<unsigned long>(index));
  snprintf(cHeld, sizeof(cHeld), "%lu", static_cast<unsigned long>(heldMs));

  Serial1.printf(kRowFormat, cIndex, stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName(), downAtStart ? "yes" : "no", cHeld,
                 msCell(cSlept, sizeof(cSlept), sleptUs, sleptValid),
                 tap ? "tap, discarded" : "question");
}

}  // namespace

void setup() {
  // The press started before this line and there is no way to find out how long
  // before: a button wake carries no deadline to measure the boot against. This
  // is the earliest honest origin for the hold.
  const int64_t tEntry = esp_timer_get_time();
  const uint64_t rtcEntry = rtc_time_get();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(tEntry);

  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  const bool sameSession = stickyPower::wokeFromDeepSleep() && g_magic == kMagic;
  const bool sleptValid = sameSession && rtcEntry > g_sleptAtTicks;
  const int64_t sleptUs =
      sleptValid ? static_cast<int64_t>(rtc_time_slowclk_to_us(
                       rtcEntry - g_sleptAtTicks, esp_clk_slowclk_cal_get()))
                 : 0;
  if (!sameSession) {
    g_magic = kMagic;
    g_presses = 0;
    printBanner();
  }

  // Before the chirp, because the chirp is 110 ms in which a short press can
  // end. A "no" here is a press that was over before the firmware could look.
  const bool downAtStart = button.isDown();

  stickyBuzzer::ready();

  uint32_t nextProgress = kProgressMs;
  while (!button.poll()) {
    if (button.heldMs() >= nextProgress) {
      Serial1.printf("  held %lu ms\n", static_cast<unsigned long>(button.heldMs()));
      nextProgress += kProgressMs;
    }
    delay(1);
  }

  printRow(++g_presses, downAtStart, button.heldMs(), sleptUs, sleptValid, button.isTap());

  if (!button.isTap()) stickyBuzzer::answer();

  Serial1.flush();
  stickyPower::prepareDeepSleep();
  // As late as possible, so the interval reported on the next wake is the sleep
  // and the boot and nothing else.
  g_sleptAtTicks = rtc_time_get();
  stickyPower::enterDeepSleep();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
