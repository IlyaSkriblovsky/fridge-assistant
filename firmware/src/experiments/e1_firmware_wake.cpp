// E1 on the finished firmware -- S9. See docs/experiments.md, E1, and
// docs/implementation.md, S9. Build and flash with:
//
//     ~/.platformio/penv/bin/pio run -e exp_e1_firmware -t upload --upload-port <port>
//
// **This rig does not replace main.cpp. It is main.cpp**, with six of its calls
// wrapped at link time. E1's own rig swaps the firmware's setup() for one of its
// own, which is the right instrument for the microphone's settle curve and the
// wrong one for the boot: the boot is the image, and the one part of the image a
// replacement cannot carry is main.cpp itself -- its globals are constructed
// before setup() runs, inside the very stage being measured. So the image here
// is the firmware's, and everything the rig adds lives either in flash, which is
// mapped rather than copied, or in RTC memory, which a wake does not reload.
//
// What the wraps add, and nothing else:
//
//   setup()                   the RTC counter and the alarm that woke the board,
//                             before the firmware's first line
//   stickyPower::holdLatch()  a timestamp as it returns
//   StickyButton::isDown()   enter capture on cold/timer wakes without a press
//   StickyMic::begin()        one on either side
//   stickyBuzzer::ready()     one as it starts -- the user is told to talk here
//   stickyPower::deepSleep()  the row goes out, and in the timer phase the sleep
//                             gets a deadline
//
// `--wrap=X` in platformio.ini sends every *undefined* reference to X to
// __wrap_X and lets __wrap_X reach the original as __real_X. Undefined means
// across object files, so it catches main.cpp calling into a module and nothing
// a module does inside itself. The names are the mangled ones because the linker
// knows no others. Every wrapper calls through to its __real_, so a function
// that is renamed or changes its arguments fails the link here instead of
// quietly going unmeasured.
//
// Two phases, as in E1's rig. First kTimerCycles timer wakes, the only kind with
// a known deadline and so the only kind with a boot number. A timer wake is the
// firmware with nobody holding the button. The isDown() wrapper bypasses only
// the startup idle check on cold/timer wakes. poll() calls isDown() within
// button.cpp, so the linker leaves it untouched: capture sees the real released
// button and discards the tap, without opening a backend request. So every row is
// a whole run of the firmware from the wake to the sleep, at the price of a full
// refresh per cycle. Then the AI button, where every row is a real question and
// the boot is a dash.
//
// Nothing is printed before the firmware's own finish() has run -- the row goes
// out from inside the deepSleep() wrapper -- so between the wake and the chirp
// the UART carries the firmware's bytes and no others.

#include <Arduino.h>
#include <esp_image_format.h>
#include <esp_ota_ops.h>
#include <esp_private/esp_clk.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <soc/rtc.h>
#include <soc/rtc_cntl_reg.h>
#include <soc/soc.h>

#include "sticky/button.h"
#include "sticky/mic.h"
#include "sticky/power.h"

// The originals and their wrappers, under the names the linker has for them.
// A member function takes `this` as its first argument in the Itanium ABI,
// which is what lets StickyMic::begin() be declared as a plain function here.
extern "C" {
void __real__Z5setupv();
void __wrap__Z5setupv();
void __real__ZN11stickyPower9holdLatchEv();
void __wrap__ZN11stickyPower9holdLatchEv();
bool __real__ZNK12StickyButton6isDownEv(const StickyButton* self);
bool __wrap__ZNK12StickyButton6isDownEv(const StickyButton* self);
bool __real__ZN9StickyMic5beginEmm(StickyMic* self, uint32_t sampleRate, uint32_t settleMs);
bool __wrap__ZN9StickyMic5beginEmm(StickyMic* self, uint32_t sampleRate, uint32_t settleMs);
void __real__ZN12stickyBuzzer5readyEv();
void __wrap__ZN12stickyBuzzer5readyEv();
[[noreturn]] void __real__ZN11stickyPower9deepSleepEy(uint64_t timerWakeUs);
[[noreturn]] void __wrap__ZN11stickyPower9deepSleepEy(uint64_t timerWakeUs);
}

namespace {

// Unattended wakes before the rig hands the board back to the button. E1's own
// rig used twenty, which is what the numbers here are compared against.
constexpr uint32_t kTimerCycles = 20;

// Short, as E1's was, so the two rigs' skews compare: whatever error the
// worked-out deadline carries from calibration scales with the sleep's length.
constexpr uint64_t kTimerSleepUs = 2000000;

constexpr uint32_t kMagic = 0xE1F1A5E0;

// What happened on one wake, from the top of setup() unless it says otherwise.
// The stages are consecutive and add up to setup-to-chirp exactly.
struct Row {
  uint32_t bootUs;   // the wake event -> setup(), on the RTC counter
  int32_t skewUs;    // E1's worked-out deadline against the programmed one
  uint32_t latchUs;  // setup() -> holdLatch() returned
  uint32_t logUs;    // holdLatch() returned -> StickyMic::begin() called
  uint32_t micUs;    // StickyMic::begin(), settle discard included
  uint32_t taskUs;   // StickyMic::begin() returned -> stickyBuzzer::ready() called
  uint8_t wakeCause;    // esp_sleep_wakeup_cause_t
  uint8_t resetReason;  // esp_reset_reason_t
  bool bootValid;
  bool chirped;  // the question got as far as the ready chirp
};

// RTC slow memory: kept through deep sleep, lost on a reset through EN -- which
// opening the serial port is, so every session starts from a clean log.
constexpr uint32_t kLogRows = 40;
RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_rows;        // recorded in all; ring index is % kLogRows
RTC_DATA_ATTR uint32_t g_timedRows;   // of those, rows with a boot number
RTC_DATA_ATTR bool g_summaryPrinted;
RTC_DATA_ATTR uint64_t g_sleepTicks;  // RTC counter as late as possible before sleeping
RTC_DATA_ATTR uint32_t g_sleepCal;    // slow clock calibration used for that reading
RTC_DATA_ATTR uint64_t g_sleepUs;     // programmed timer duration, 0 for a button sleep
RTC_DATA_ATTR Row g_log[kLogRows];

// This wake. esp_timer is monotonic within a boot, which is all these need.
bool g_newSession = false;
Row g_row{};
int64_t g_entryUs = 0;
int64_t g_latchUs = 0;
int64_t g_micInUs = 0;
int64_t g_micOutUs = 0;
int64_t g_chirpUs = 0;

// The alarm the sleep was actually programmed with. esp_deep_sleep_start()
// writes it into RTC_CNTL, which is the domain that stays up through the sleep
// and is not reset by the wake, so it is still there at the top of setup() --
// the exact RTC tick the wake event happened at.
uint64_t programmedAlarm() {
  return (static_cast<uint64_t>(REG_GET_FIELD(RTC_CNTL_SLP_TIMER1_REG, RTC_CNTL_SLP_VAL_HI))
          << 32) |
         REG_READ(RTC_CNTL_SLP_TIMER0_REG);
}

// Slow-clock ticks to microseconds, either sign.
int32_t signedTicksUs(int64_t ticks, uint32_t cal) {
  const uint64_t us = rtc_time_slowclk_to_us(static_cast<uint64_t>(ticks < 0 ? -ticks : ticks), cal);
  return ticks < 0 ? -static_cast<int32_t>(us) : static_cast<int32_t>(us);
}

// The boot: the RTC counter now, less the alarm that woke the board. esp_timer
// cannot do this -- it restarts on every wake and does not carry the sleep --
// and the RTC counter can, because the alarm is in the same ticks. Converting
// only the short difference keeps the calibration error off the absolute
// counter, which is hours wide.
//
// E1's rig worked the alarm out instead of reading it: the counter just before
// esp_deep_sleep_start() plus the duration, converted with the calibration of
// the boot before. That misses whatever the sleep entry does before it reads
// the counter itself, IDF's overhead compensation, and any difference between
// the calibration it used and the one IDF did. `skew` is how far that worked-out
// deadline was from the programmed one, so the old method can be judged against
// the register on every row: old boot = boot + skew.
bool bootTimeUs(uint64_t rtcAtEntry, uint32_t& bootUs, int32_t& skewUs) {
  if (!stickyPower::wokeFromDeepSleep() || g_magic != kMagic || g_sleepUs == 0) return false;
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return false;

  const uint64_t alarm = programmedAlarm();
  if (rtcAtEntry <= alarm) return false;
  const uint32_t cal = esp_clk_slowclk_cal_get();
  bootUs = static_cast<uint32_t>(rtc_time_slowclk_to_us(rtcAtEntry - alarm, cal));

  const uint64_t workedOut = g_sleepTicks + rtc_time_us_to_slowclk(g_sleepUs, g_sleepCal);
  skewUs = signedTicksUs(static_cast<int64_t>(alarm) - static_cast<int64_t>(workedOut), cal);
  return true;
}

uint32_t spanUs(int64_t from, int64_t to) {
  return from != 0 && to >= from ? static_cast<uint32_t>(to - from) : 0;
}

// What the bootloader copies on a wake: the segments that load into internal
// RAM. Flash-resident code and constants are mapped rather than copied, and
// the RTC segments are skipped on a deep sleep wake -- which is why the rig's
// own log costs the measurement nothing.
void printImage() {
  const esp_partition_t* running = esp_ota_get_running_partition();
  esp_image_metadata_t meta{};
  if (running == nullptr) {
    Serial1.println("  image: no running partition to read");
    return;
  }
  const esp_partition_pos_t pos{running->address, running->size};
  if (esp_image_get_metadata(&pos, &meta) != ESP_OK) {
    Serial1.println("  image: could not read the running image's header");
    return;
  }

  uint32_t iram = 0;
  uint32_t dram = 0;
  for (uint32_t i = 0; i < meta.image.segment_count; ++i) {
    const uint32_t at = meta.segments[i].load_addr;
    const uint32_t len = meta.segments[i].data_len;
    if (at >= SOC_IRAM_LOW && at < SOC_IRAM_HIGH) iram += len;
    if (at >= SOC_DRAM_LOW && at < SOC_DRAM_HIGH) dram += len;
  }
  Serial1.printf("  image %lu bytes; a wake copies %lu of IRAM and %lu of DRAM\n",
                 static_cast<unsigned long>(meta.image_len), static_cast<unsigned long>(iram),
                 static_cast<unsigned long>(dram));
}

void printBanner() {
  Serial1.println();
  Serial1.printf("E1 on the firmware -- S9 (session began on %s)\n",
                 stickyPower::resetReasonName(static_cast<esp_reset_reason_t>(g_row.resetReason)));
  printImage();
  Serial1.printf("  %lu timer wakes of %lu ms, then the AI button; every wake is a whole run\n",
                 static_cast<unsigned long>(kTimerCycles),
                 static_cast<unsigned long>(kTimerSleepUs / 1000));
  Serial1.println("  of the firmware, and the chirp at the start of each is its ready chirp");
}

// One cell: a dash where the number does not exist, rather than a zero that
// reads as "instant".
const char* cell(char* buf, size_t size, uint32_t us, bool valid) {
  if (valid) {
    snprintf(buf, size, "%.1f", us / 1000.0f);
  } else {
    snprintf(buf, size, "--");
  }
  return buf;
}

const char* signedCell(char* buf, size_t size, int32_t us, bool valid) {
  if (valid) {
    snprintf(buf, size, "%+.1f", us / 1000.0f);
  } else {
    snprintf(buf, size, "--");
  }
  return buf;
}

uint32_t setupUs(const Row& r) { return r.latchUs + r.logUs + r.micUs + r.taskUs; }

constexpr const char* kRowFormat = "  %3s  %-5s %-9s %7s %7s %7s %7s %7s %7s %7s %7s\n";

void printHeader() {
  Serial1.printf(kRowFormat, "#", "wake", "reset", "boot", "skew", "latch", "log", "mic",
                 "task", "setup", "total");
}

void printRow(uint32_t index, const Row& r) {
  char cIndex[8], cBoot[12], cSkew[12], cLatch[12], cLog[12], cMic[12], cTask[12];
  char cSetup[12], cTotal[12];
  snprintf(cIndex, sizeof(cIndex), "%lu", static_cast<unsigned long>(index));
  Serial1.printf(
      kRowFormat, cIndex,
      stickyPower::wakeupCauseName(static_cast<esp_sleep_wakeup_cause_t>(r.wakeCause)),
      stickyPower::resetReasonName(static_cast<esp_reset_reason_t>(r.resetReason)),
      cell(cBoot, sizeof(cBoot), r.bootUs, r.bootValid),
      signedCell(cSkew, sizeof(cSkew), r.skewUs, r.bootValid),
      cell(cLatch, sizeof(cLatch), r.latchUs, r.chirped),
      cell(cLog, sizeof(cLog), r.logUs, r.chirped),
      cell(cMic, sizeof(cMic), r.micUs, r.chirped),
      cell(cTask, sizeof(cTask), r.taskUs, r.chirped),
      cell(cSetup, sizeof(cSetup), setupUs(r), r.chirped),
      cell(cTotal, sizeof(cTotal), r.bootUs + setupUs(r), r.bootValid && r.chirped));
}

float median(float* values, uint32_t count) {
  for (uint32_t i = 1; i < count; ++i) {
    const float key = values[i];
    uint32_t j = i;
    while (j > 0 && values[j - 1] > key) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = key;
  }
  return values[count / 2];
}

// Min, median and max of one column over the rows that have a boot number --
// the timer phase, which is the only set of rows taken the same way.
void summarise(const char* label, float (*pick)(const Row&)) {
  static float scratch[kLogRows];
  const uint32_t kept = g_rows < kLogRows ? g_rows : kLogRows;
  uint32_t count = 0;
  float lo = 0.0f;
  float hi = 0.0f;
  for (uint32_t i = g_rows - kept; i < g_rows; ++i) {
    const Row& r = g_log[i % kLogRows];
    if (!r.bootValid || !r.chirped) continue;
    const float ms = pick(r);
    if (count == 0 || ms < lo) lo = ms;
    if (count == 0 || ms > hi) hi = ms;
    scratch[count++] = ms;
  }
  if (count == 0) return;
  Serial1.printf("    %-6s min %7.1f  median %7.1f  max %7.1f  (n=%lu)\n", label, lo,
                 median(scratch, count), hi, static_cast<unsigned long>(count));
}

void printSummary() {
  const uint32_t kept = g_rows < kLogRows ? g_rows : kLogRows;
  Serial1.println();
  Serial1.printf("E1 on the firmware: %lu wakes, %lu with a boot number, milliseconds\n",
                 static_cast<unsigned long>(g_rows), static_cast<unsigned long>(g_timedRows));
  printHeader();
  for (uint32_t i = g_rows - kept; i < g_rows; ++i) printRow(i, g_log[i % kLogRows]);

  Serial1.println();
  summarise("boot", [](const Row& r) { return r.bootUs / 1000.0f; });
  summarise("skew", [](const Row& r) { return r.skewUs / 1000.0f; });
  summarise("latch", [](const Row& r) { return r.latchUs / 1000.0f; });
  summarise("log", [](const Row& r) { return r.logUs / 1000.0f; });
  summarise("mic", [](const Row& r) { return r.micUs / 1000.0f; });
  summarise("task", [](const Row& r) { return r.taskUs / 1000.0f; });
  summarise("setup", [](const Row& r) { return setupUs(r) / 1000.0f; });
  summarise("total", [](const Row& r) { return (r.bootUs + setupUs(r)) / 1000.0f; });

  Serial1.println();
  Serial1.println("  boot  the wake event to setup(), from the alarm in RTC_CNTL");
  Serial1.println("  skew  E1's worked-out deadline less the programmed one: its boot = boot+skew");
  Serial1.println("  latch holdLatch(): delay(100) on a cold start, nothing on a wake");
  Serial1.println("  log   the button, Serial1 and the log lines ahead of the microphone");
  Serial1.println("  mic   rail, USB PHY, I2S start and the settle discard");
  Serial1.println("  task  the PSRAM buffer and the capture task");
  Serial1.println("  setup = latch+log+mic+task, setup() to the ready chirp");
  Serial1.println("  total = boot+setup, the wake event to the ready chirp");
  Serial1.println();
  Serial1.println("Timer phase over. Every wake from here is the AI button, and a real question.");
}

}  // namespace

void __wrap__Z5setupv() {
  // Before anything, the firmware's first line included. The latch comes a few
  // microseconds later than it would without the rig -- the pads are still
  // held from the sleep, so nothing is at risk -- and that is the only thing on
  // the wake path the rig moves.
  g_entryUs = esp_timer_get_time();
  const uint64_t rtcEntry = rtc_time_get();

  g_row = Row{};
  g_row.wakeCause = static_cast<uint8_t>(esp_sleep_get_wakeup_cause());
  g_row.resetReason = static_cast<uint8_t>(esp_reset_reason());
  g_row.bootValid = bootTimeUs(rtcEntry, g_row.bootUs, g_row.skewUs);

  if (!stickyPower::wokeFromDeepSleep() || g_magic != kMagic) {
    g_magic = kMagic;
    g_rows = 0;
    g_timedRows = 0;
    g_summaryPrinted = false;
    g_sleepUs = 0;
    g_newSession = true;
  }

  __real__Z5setupv();
}

void __wrap__ZN11stickyPower9holdLatchEv() {
  __real__ZN11stickyPower9holdLatchEv();
  if (g_latchUs == 0) g_latchUs = esp_timer_get_time();
}

bool __wrap__ZNK12StickyButton6isDownEv(const StickyButton* self) {
  // Only main.cpp's startup check crosses the object-file boundary. Leave
  // real button wakes unchanged, including taps released before setup.
  const bool down = __real__ZNK12StickyButton6isDownEv(self);
  return down || g_newSession || g_row.wakeCause == ESP_SLEEP_WAKEUP_TIMER;
}

bool __wrap__ZN9StickyMic5beginEmm(StickyMic* self, uint32_t sampleRate, uint32_t settleMs) {
  const int64_t inUs = esp_timer_get_time();
  const bool ok = __real__ZN9StickyMic5beginEmm(self, sampleRate, settleMs);
  if (g_micInUs == 0) {
    g_micInUs = inUs;
    g_micOutUs = esp_timer_get_time();
  }
  return ok;
}

void __wrap__ZN12stickyBuzzer5readyEv() {
  if (g_chirpUs == 0) g_chirpUs = esp_timer_get_time();
  __real__ZN12stickyBuzzer5readyEv();
}

// The firmware's one exit. It never asks for a timer; the rig adds one while the
// timer phase lasts, and after that sleeps exactly as the firmware asked.
void __wrap__ZN11stickyPower9deepSleepEy(uint64_t timerWakeUs) {
  g_row.chirped = g_latchUs != 0 && g_micInUs != 0 && g_chirpUs != 0;
  if (g_row.chirped) {
    g_row.latchUs = spanUs(g_entryUs, g_latchUs);
    g_row.logUs = spanUs(g_latchUs, g_micInUs);
    g_row.micUs = spanUs(g_micInUs, g_micOutUs);
    g_row.taskUs = spanUs(g_micOutUs, g_chirpUs);
  }

  const uint32_t index = g_rows;
  g_log[index % kLogRows] = g_row;
  ++g_rows;
  if (g_row.bootValid) ++g_timedRows;

  if (g_newSession) printBanner();
  Serial1.println();
  printHeader();
  printRow(index, g_row);

  if (g_timedRows < kTimerCycles) {
    Serial1.flush();
    stickyPower::prepareDeepSleep(kTimerSleepUs);

    // Taken as late as possible: everything between here and the deadline
    // latched inside esp_deep_sleep_start() lands in the next boot number.
    g_sleepUs = kTimerSleepUs;
    g_sleepCal = esp_clk_slowclk_cal_get();
    g_sleepTicks = rtc_time_get();
    stickyPower::enterDeepSleep();
  }

  if (!g_summaryPrinted) {
    printSummary();
    g_summaryPrinted = true;
  }
  Serial1.flush();
  g_sleepUs = 0;
  __real__ZN11stickyPower9deepSleepEy(timerWakeUs);
}
