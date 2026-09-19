// E1 -- wake-to-first-sample latency, and E3 -- microphone settle window.
// See docs/experiments.md. Build and flash with:
//
//     ~/.platformio/penv/bin/pio run -e exp_e1
//     ~/.platformio/penv/bin/pio run -e exp_e1 -t upload --upload-port <port>
//
// This replaces main.cpp in its own environment; nothing here is part of the
// firmware. What the rig measures, per wake:
//
//   boot    the deep sleep wake event -> the first line of setup()
//   latch   stickyPower::holdLatch()
//   mic     mic rail, USB PHY release, I2S PDM-RX start (mic's own delay(10))
//   block   until the first block of PCM is in hand
//   settle  from that block until the level stops moving -- E3
//
// Two phases. First kTimerCycles unattended wakes on the timer, which is the
// only way to get the boot number: the wake deadline is known, so the boot time
// is the RTC counter at the top of setup() minus that deadline. Then button
// wakes, which take the real ext1 path but carry no known deadline, so they
// confirm the stages and the wake cause rather than the boot time. Holding the
// button through the report is E4.
//
// Nothing is printed until the capture is over. At 115200 baud a log line is
// several milliseconds, injected straight into the path being measured.
//
// Running on battery. The serial console only exists over USB, which feeds the
// rail and hides exactly the failure the latch code has to be checked against,
// so the rig does not depend on anyone reading it:
//
//   * every wake ends in a chirp, after the capture so it pollutes neither the
//     timings nor the audio. On battery that is the aliveness signal -- twenty
//     chirps two seconds apart means twenty deep sleep cycles survived, and
//     silence means the board switched itself off;
//   * every cycle is kept in RTC memory, and holding the button for kDumpHoldMs
//     prints the whole log. That does not rescue a battery run: opening the
//     serial monitor resets the board through EN, which clears RTC memory, so
//     the log is gone before there is a console to print it to. A battery run
//     gives chirps and nothing else; numbers off battery would need NVS. The
//     dump is still the quick way to re-read a run that is already on USB.

#include <Arduino.h>
#include <driver/rtc_io.h>
#include <esp_private/esp_clk.h>
#include <esp_sleep.h>
#include <soc/rtc.h>
#include <soc/rtc_cntl_reg.h>

#include "sticky/mic.h"
#include "sticky/power.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// Buzzer, per the pin map in docs/project-vision.md. Driven through LEDC rather
// than tone(), which hands the note to a background task and would still be
// sounding when the rig goes back to sleep.
constexpr int kPinBuzzer = 48;
constexpr uint32_t kChirpHz = 3000;
constexpr uint32_t kChirpMs = 60;

// Unattended wakes before the rig switches to the button. Enough to see the
// spread; a single sample is not a measurement.
constexpr uint32_t kTimerCycles = 20;

// Short on purpose. Whatever error the worked-out deadline -- the skew column --
// carries from calibration scales with how long the sleep was.
constexpr uint64_t kTimerSleepUs = 2000000;

// Held at least this long, a button press means "print the whole log". Well
// above any plausible push-to-talk press, so it cannot be hit by accident.
constexpr uint32_t kDumpHoldMs = 3000;

// 128 samples is an 8 ms block at 16 kHz: short enough to place the first
// sample and to resolve the settle curve, long enough for a stable RMS.
constexpr uint32_t kBlockSamples = 128;
constexpr uint32_t kCurveBlocks = 48;  // ~384 ms of audio, well past the settle

// A block counts as settled once it is within this of the noise floor and stays
// there for the rest of the curve.
constexpr float kSettleBandDb = 3.0f;

constexpr uint32_t kMagic = 0xE1C0FFEE;

// Header and rows go through the same format string, so the columns line up.
// Every field is a string: a stage that does not apply prints a dash.
constexpr const char* kRowFormat =
    "%3s  %-5s %-9s %7s %7s %7s %7s %7s %7s %7s %7s %7s %7s\n";

struct Cycle {
  uint32_t bootUs;
  int32_t skewUs;  // the worked-out deadline against the programmed one
  uint32_t latchUs;
  uint32_t micUs;
  uint32_t blockUs;
  uint32_t settleUs;
  float firstDbfs;
  float floorDbfs;
  uint8_t wakeCause;    // esp_sleep_wakeup_cause_t, as recorded on that wake
  uint8_t resetReason;  // esp_reset_reason_t, likewise
  bool bootValid;
  bool settleValid;
  bool micOk;
};

// RTC slow memory survives deep sleep and a reset, but not a power cycle --
// which is exactly the session boundary this wants. A board that dropped its
// latch and switched itself off comes back with the log gone, and that absence
// is itself the result.
constexpr uint32_t kLogCycles = 64;
RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_logCount;  // total recorded; ring index is % kLogCycles
RTC_DATA_ATTR uint32_t g_buttonWakes;
RTC_DATA_ATTR uint64_t g_sleepTicks;  // RTC counter as late as possible before sleeping
RTC_DATA_ATTR uint32_t g_sleepCal;    // slow clock calibration used for that reading
RTC_DATA_ATTR uint64_t g_sleepUs;     // programmed timer duration, 0 for a button sleep
RTC_DATA_ATTR Cycle g_log[kLogCycles];

StickyMic mic;

int64_t g_curveTs[kCurveBlocks];
float g_curveDbfs[kCurveBlocks];
uint32_t g_curveCount;

// Blocking on purpose, and it leaves the pad low so the parking in
// prepareDeepSleep() holds it there.
void chirp() {
  if (!ledcAttach(kPinBuzzer, kChirpHz, 10)) return;
  ledcWriteTone(kPinBuzzer, kChirpHz);
  delay(kChirpMs);
  ledcWriteTone(kPinBuzzer, 0);
  ledcDetach(kPinBuzzer);
  pinMode(kPinBuzzer, OUTPUT);
  digitalWrite(kPinBuzzer, LOW);
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

void summarise(const char* label, const uint32_t* values, uint32_t count) {
  if (count == 0) return;
  static float scratch[kTimerCycles];
  uint32_t lo = values[0];
  uint32_t hi = values[0];
  for (uint32_t i = 0; i < count; ++i) {
    scratch[i] = values[i] / 1000.0f;
    if (values[i] < lo) lo = values[i];
    if (values[i] > hi) hi = values[i];
  }
  Serial1.printf("  %-10s min %7.1f  median %7.1f  max %7.1f  (n=%lu)\n", label,
                 lo / 1000.0f, median(scratch, count), hi / 1000.0f,
                 static_cast<unsigned long>(count));
}

// The boot half of the measurement. esp_timer is useless for this: it restarts
// on every wake and does not carry the sleep, so it reads the same small value
// at the top of every setup() however long the board slept, and it cannot see a
// boot that ran before it started.
//
// The RTC counter works because the alarm is in the same ticks. Working in ticks
// and converting only the short difference keeps the calibration error off the
// absolute counter, which is hours wide by then.
//
// **The alarm is read, not worked out.** esp_deep_sleep_start() writes it into
// RTC_CNTL, which stays up through the sleep and is not reset by the wake, so
// the register holds the exact tick the wake event happened at. Working it out
// -- the counter just before esp_deep_sleep_start() plus the duration,
// converted with the previous boot's calibration -- misses what the sleep entry
// does before reading the counter itself, IDF's overhead compensation, and any
// difference between the calibration it used and the one IDF did. `skew` is the
// programmed alarm less the worked-out one, so a boot worked out that way is
// boot + skew.
uint64_t programmedAlarm() {
  return (static_cast<uint64_t>(REG_GET_FIELD(RTC_CNTL_SLP_TIMER1_REG, RTC_CNTL_SLP_VAL_HI))
          << 32) |
         REG_READ(RTC_CNTL_SLP_TIMER0_REG);
}

bool bootTimeUs(uint64_t rtcAtEntry, uint32_t& out, int32_t& skewUs) {
  if (!stickyPower::wokeFromDeepSleep() || g_magic != kMagic || g_sleepUs == 0) {
    return false;
  }
  if (esp_sleep_get_wakeup_cause() != ESP_SLEEP_WAKEUP_TIMER) return false;

  const uint64_t alarm = programmedAlarm();
  if (rtcAtEntry <= alarm) return false;
  const uint32_t cal = esp_clk_slowclk_cal_get();
  out = static_cast<uint32_t>(rtc_time_slowclk_to_us(rtcAtEntry - alarm, cal));

  const uint64_t workedOut = g_sleepTicks + rtc_time_us_to_slowclk(g_sleepUs, g_sleepCal);
  const int64_t skewTicks = static_cast<int64_t>(alarm) - static_cast<int64_t>(workedOut);
  const uint64_t skewAbs = rtc_time_slowclk_to_us(
      static_cast<uint64_t>(skewTicks < 0 ? -skewTicks : skewTicks), cal);
  skewUs = skewTicks < 0 ? -static_cast<int32_t>(skewAbs) : static_cast<int32_t>(skewAbs);
  return true;
}

void printHeader() {
  Serial1.printf(kRowFormat, "#", "wake", "reset", "boot", "skew", "latch", "mic", "block",
                 "settle", "dead", "total", "first", "floor");
}

void printBanner() {
  Serial1.println();
  Serial1.println("E1 wake latency rig");
  Serial1.printf("  sketch %lu bytes, flash mode %d at %lu MHz, cpu %lu MHz\n",
                 static_cast<unsigned long>(ESP.getSketchSize()),
                 static_cast<int>(ESP.getFlashChipMode()),
                 static_cast<unsigned long>(ESP.getFlashChipSpeed() / 1000000),
                 static_cast<unsigned long>(getCpuFrequencyMhz()));
  Serial1.printf("  %lu timer cycles of %lu ms, then the AI button\n",
                 static_cast<unsigned long>(kTimerCycles),
                 static_cast<unsigned long>(kTimerSleepUs / 1000));
  Serial1.printf("  a chirp per wake; hold the button %lu ms to print the log\n",
                 static_cast<unsigned long>(kDumpHoldMs));
  Serial1.println();
  printHeader();
}

// One cell, so every column in the table is built the same way: a dash where a
// number does not exist rather than a misleading zero.
const char* cell(char* buf, size_t size, float value, bool valid) {
  if (valid) {
    snprintf(buf, size, "%.1f", value);
  } else {
    snprintf(buf, size, "--");
  }
  return buf;
}

void printCycle(uint32_t index, const Cycle& c) {
  char cIndex[8], cBoot[10], cSkew[10], cLatch[10], cMic[10], cBlock[10], cSettle[10];
  char cDead[10], cTotal[10], cFirst[10], cFloor[10];

  snprintf(cIndex, sizeof(cIndex), "%lu", static_cast<unsigned long>(index));
  const uint32_t dead = c.bootUs + c.latchUs + c.micUs + c.blockUs;

  Serial1.printf(
      kRowFormat, cIndex,
      stickyPower::wakeupCauseName(static_cast<esp_sleep_wakeup_cause_t>(c.wakeCause)),
      stickyPower::resetReasonName(static_cast<esp_reset_reason_t>(c.resetReason)),
      cell(cBoot, sizeof(cBoot), c.bootUs / 1000.0f, c.bootValid),
      cell(cSkew, sizeof(cSkew), c.skewUs / 1000.0f, c.bootValid),
      cell(cLatch, sizeof(cLatch), c.latchUs / 1000.0f, true),
      cell(cMic, sizeof(cMic), c.micUs / 1000.0f, true),
      cell(cBlock, sizeof(cBlock), c.blockUs / 1000.0f, c.micOk),
      cell(cSettle, sizeof(cSettle), c.settleUs / 1000.0f, c.settleValid),
      cell(cDead, sizeof(cDead), dead / 1000.0f, c.bootValid && c.micOk),
      cell(cTotal, sizeof(cTotal), (dead + c.settleUs) / 1000.0f,
           c.bootValid && c.settleValid),
      cell(cFirst, sizeof(cFirst), c.firstDbfs, c.micOk),
      cell(cFloor, sizeof(cFloor), c.floorDbfs, c.micOk));
}

void printCurve() {
  Serial1.printf("  settle curve, %lu ms blocks (block, ms after the first, dBFS):\n",
                 static_cast<unsigned long>(kBlockSamples * 1000 / StickyMic::kSampleRate));
  for (uint32_t i = 0; i < g_curveCount; ++i) {
    Serial1.printf("    %2lu %7.1f %8.1f\n", static_cast<unsigned long>(i),
                   (g_curveTs[i] - g_curveTs[0]) / 1000.0f, g_curveDbfs[i]);
  }
}

uint32_t recordCycle(const Cycle& c) {
  const uint32_t index = g_logCount;
  g_log[index % kLogCycles] = c;
  ++g_logCount;
  return index;
}

// Every cycle still in the ring, oldest first. This is the whole point of the
// battery run: nobody is reading the console while it happens.
void printLog() {
  if (g_logCount == 0) {
    Serial1.println("log is empty");
    return;
  }
  const uint32_t kept = g_logCount < kLogCycles ? g_logCount : kLogCycles;
  Serial1.printf("\n%lu cycles recorded, %lu still in the ring\n",
                 static_cast<unsigned long>(g_logCount),
                 static_cast<unsigned long>(kept));
  printHeader();
  for (uint32_t i = g_logCount - kept; i < g_logCount; ++i) {
    printCycle(i, g_log[i % kLogCycles]);
  }
  Serial1.println();
}

void printSummary() {
  static uint32_t scratch[kTimerCycles];
  const uint32_t cycles = g_logCount < kTimerCycles ? g_logCount : kTimerCycles;
  uint32_t count = 0;

  Serial1.println();
  Serial1.printf("Summary over %lu cycles, milliseconds\n",
                 static_cast<unsigned long>(cycles));

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) {
    if (g_log[i].bootValid) scratch[count++] = g_log[i].bootUs;
  }
  summarise("boot", scratch, count);

  {
    static float skews[kTimerCycles];
    uint32_t n = 0;
    float lo = 0.0f;
    float hi = 0.0f;
    for (uint32_t i = 0; i < cycles; ++i) {
      if (!g_log[i].bootValid) continue;
      const float ms = g_log[i].skewUs / 1000.0f;
      if (n == 0 || ms < lo) lo = ms;
      if (n == 0 || ms > hi) hi = ms;
      skews[n++] = ms;
    }
    if (n > 0) {
      Serial1.printf("  %-10s min %+7.1f  median %+7.1f  max %+7.1f  (n=%lu)\n", "skew", lo,
                     median(skews, n), hi, static_cast<unsigned long>(n));
    }
  }

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) scratch[count++] = g_log[i].latchUs;
  summarise("latch", scratch, count);

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) scratch[count++] = g_log[i].micUs;
  summarise("mic", scratch, count);

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) scratch[count++] = g_log[i].blockUs;
  summarise("block", scratch, count);

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) {
    if (g_log[i].settleValid) scratch[count++] = g_log[i].settleUs;
  }
  summarise("settle", scratch, count);

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) {
    const Cycle& c = g_log[i];
    if (!c.bootValid || !c.micOk) continue;
    scratch[count++] = c.bootUs + c.latchUs + c.micUs + c.blockUs;
  }
  summarise("dead", scratch, count);

  count = 0;
  for (uint32_t i = 0; i < cycles; ++i) {
    const Cycle& c = g_log[i];
    if (!c.bootValid || !c.settleValid) continue;
    scratch[count++] = c.bootUs + c.latchUs + c.micUs + c.blockUs + c.settleUs;
  }
  summarise("total", scratch, count);

  Serial1.println();
  Serial1.println("  latch is delay(100) on a cold start and nothing on a wake.");
  Serial1.println("  dead = boot+latch+mic+block; total adds the settle window.");
  Serial1.println("  boot is from the alarm in RTC_CNTL; boot+skew is the old worked-out one.");
  Serial1.println();
  Serial1.println("Hold the AI button to wake. Boot is not measurable on that path,");
  Serial1.println("but the stages and the wake cause are -- and a long hold is E4.");
}

void sleepAgain(uint64_t timerUs) {
  Serial1.flush();
  stickyPower::prepareDeepSleep(timerUs);

  // Taken as late as possible: everything between here and the deadline latched
  // inside esp_deep_sleep_start() lands in the boot number.
  g_sleepUs = timerUs;
  g_sleepCal = esp_clk_slowclk_cal_get();
  g_sleepTicks = rtc_time_get();
  g_magic = kMagic;
  stickyPower::enterDeepSleep();
}

// How long the AI button stays down, counted from the top of setup() since the
// press is what woke the board and the capture has already run. Firmware only
// reads the pin; if the hardware reacts to a long hold, the board dies here and
// the log stops -- which is the E4 answer.
uint32_t reportHold(int64_t tEntry) {
  rtc_gpio_deinit(static_cast<gpio_num_t>(stickyPower::kPinAiButton));
  pinMode(stickyPower::kPinAiButton, INPUT_PULLUP);

  int64_t nextReport = 5000000;
  for (;;) {
    const int64_t held = esp_timer_get_time() - tEntry;
    if (digitalRead(stickyPower::kPinAiButton) != LOW) {
      Serial1.printf("  released after %lld ms\n", held / 1000);
      return static_cast<uint32_t>(held / 1000);
    }
    if (held >= nextReport) {
      Serial1.printf("  held %lld ms, board alive\n", held / 1000);
      nextReport += 5000000;
    }
    if (held > 60000000) {
      Serial1.println("  still held at 60 s, giving up on the release");
      return static_cast<uint32_t>(held / 1000);
    }
    delay(5);
  }
}

}  // namespace

void setup() {
  // esp_timer is monotonic within a boot, which is all the stage deltas below
  // need. Its absolute value is meaningless here -- see bootTimeUs().
  const int64_t tEntry = esp_timer_get_time();
  const uint64_t rtcEntry = rtc_time_get();

  Cycle cycle{};
  cycle.wakeCause = static_cast<uint8_t>(esp_sleep_get_wakeup_cause());
  cycle.resetReason = static_cast<uint8_t>(esp_reset_reason());
  cycle.bootValid = bootTimeUs(rtcEntry, cycle.bootUs, cycle.skewUs);

  stickyPower::holdLatch();
  const int64_t tLatch = esp_timer_get_time();

  cycle.micOk = mic.begin(StickyMic::kSampleRate, /*settleMs=*/0);
  const int64_t tMic = esp_timer_get_time();

  g_curveCount = 0;
  if (cycle.micOk) {
    MicLevel level;
    for (uint32_t i = 0; i < kCurveBlocks; ++i) {
      if (!mic.readLevel(level, kBlockSamples)) break;
      g_curveTs[i] = esp_timer_get_time();
      g_curveDbfs[i] = level.dbfs;
      ++g_curveCount;
    }
  }
  mic.end();

  cycle.latchUs = static_cast<uint32_t>(tLatch - tEntry);
  cycle.micUs = static_cast<uint32_t>(tMic - tLatch);

  if (g_curveCount > 0) {
    cycle.blockUs = static_cast<uint32_t>(g_curveTs[0] - tMic);
    cycle.firstDbfs = g_curveDbfs[0];

    // The floor is the median of the tail, and the settle point is the first
    // block after the last excursion out of the band around it.
    const uint32_t tail = g_curveCount >= 8 ? 8 : g_curveCount;
    static float scratch[kCurveBlocks];
    for (uint32_t i = 0; i < tail; ++i) scratch[i] = g_curveDbfs[g_curveCount - tail + i];
    cycle.floorDbfs = median(scratch, tail);

    uint32_t settled = 0;
    for (uint32_t i = g_curveCount; i > 0; --i) {
      if (fabsf(g_curveDbfs[i - 1] - cycle.floorDbfs) > kSettleBandDb) {
        settled = i;
        break;
      }
    }
    // settled == g_curveCount means the level was still moving when the curve
    // ran out -- someone talking over it, most likely. Reporting zero there
    // would read as "settled immediately", which is the opposite.
    cycle.settleValid = settled < g_curveCount;
    cycle.settleUs = cycle.settleValid
                         ? static_cast<uint32_t>(g_curveTs[settled] - g_curveTs[0])
                         : 0;
  }

  // Audible proof of life for the battery run. After every measurement is taken
  // and the microphone is off, so it costs no time and records nothing.
  chirp();

  // Only now is it safe to spend time on the UART.
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  if (!stickyPower::wokeFromDeepSleep() || g_magic != kMagic) {
    // Not a wake: either the first run after flashing, or something reset the
    // board mid-session -- plugging USB back in after a battery run, most
    // likely. If there is a log, it belongs to that interrupted session and
    // this is the only chance to print it.
    if (g_magic == kMagic && g_logCount > 0) {
      Serial1.println();
      Serial1.printf("Interrupted session recovered from RTC memory (%s)\n",
                     stickyPower::resetReasonName());
      printLog();
    }
    g_magic = kMagic;
    g_logCount = 0;
    g_buttonWakes = 0;
    printBanner();
  }

  if (!cycle.micOk) {
    Serial1.printf("microphone begin FAILED: %s\n", mic.lastError());
  }

  const uint32_t index = recordCycle(cycle);
  printCycle(index, cycle);

  const bool buttonWake =
      static_cast<esp_sleep_wakeup_cause_t>(cycle.wakeCause) == ESP_SLEEP_WAKEUP_EXT1;
  const bool timerPhase = g_logCount <= kTimerCycles;

  if (index <= 1 || (buttonWake && g_buttonWakes == 0)) printCurve();

  // A press is answered wherever it lands: the timer phase sleeps with ext1
  // armed too.
  if (buttonWake) {
    ++g_buttonWakes;
    if (reportHold(tEntry) >= kDumpHoldMs) printLog();
  }

  if (timerPhase) {
    if (g_logCount < kTimerCycles) sleepAgain(kTimerSleepUs);
    printSummary();
  }
  sleepAgain(0);
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
