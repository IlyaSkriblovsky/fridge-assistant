// E8 -- what the 2.4 s of a refresh is made of. See docs/experiments.md. Build
// and flash with:
//
//     ~/.platformio/penv/bin/pio run -e exp_e8
//     ~/.platformio/penv/bin/pio run -e exp_e8 -t upload --upload-port <port>
//
// This replaces main.cpp in its own environment; nothing here is part of the
// firmware. S5 timed the three screens from the outside and got 2373-2446 ms,
// deterministic to the millisecond, and every decision since -- D4's display
// task, D6's partial refresh, the working screen at S8 -- has argued about that
// number as if it were one thing. It is not. Between `refresh()` and its return
// the panel is reset and re-initialised, two 48000-byte planes go out over SPI
// at 10 MHz, a waveform runs, the controller powers its analog side down, and
// then two separate delay(100) calls run back to back with the image already on
// the glass. This rig puts a number on each of those.
//
// **Seven phases, timed from inside the driver.** Panel_EPaper drives the
// controller through IDriver's virtuals, so a subclass of the firmware's own
// Driver_SSD1677_Sticky sees every one of them without the library being
// touched -- which is the same rule src/sticky/epaper.h follows and the reason
// the corrections in it are a subclass rather than a patch:
//
//   cpu     everything Panel_EPaper does to the frame buffer: padding, the
//           horizontal-mirror flip of both planes, the allocations and the
//           memcpy of the previous-frame snapshot. The one phase that is our
//           code rather than the panel's, and the residual of all the others.
//   wake    wake() -- hardwareReset(10,10), software reset 0x12, the register
//           block, and for a partial the extra initPartial() on top.
//   push    the plane or planes going out over SPI: 0x26 and 0x24 for a full
//           refresh, the shadow and 0x24 for a partial.
//   drive   0x22 with the power-down steps taken out, then 0x20, then BUSY.
//           The waveform, and nothing else.
//   power   0x22 = 0x03 then 0x20 then BUSY: disable analog, disable OSC. This
//           is the controller shutting down after the image is already drawn.
//   sleep   the driver's own sleep(): 0x10/0x01 and its delay(100).
//   tail    from the end of that sleep() to the return of refresh(), which is
//           Panel_EPaper::ePaperSleep() adding a *second* delay(100) on top of
//           the driver's. It is private and non-virtual, so it cannot be
//           subclassed away -- only measured, and then argued about upstream.
//
// **The split is checked rather than believed.** Byte 0x22 is a list of steps
// the controller runs when 0x20 arrives, and taking the last two out of it --
// 0xF7 -> 0xF4 for a full refresh, 0xFF -> 0xFC for a partial -- is what
// separates "the waveform ran" from "the analog side shut down". That reading
// of the bits comes from the SSD168x family and has not been confirmed against
// an SSD1677 datasheet, so every refresh is run both ways, alternating, and the
// rig reports whether the split total agrees with the unsplit one. If it does
// not, the decomposition is wrong and the power column is not a number.
//
// **BUSY is also traced by interrupt**, both edges, into a ring buffer the rig
// prints for the first refresh of each shape. The library polls that pin every
// millisecond and so can only see the last falling edge; an interrupt sees
// whether the controller drops BUSY between phases, which would say more about
// the inside of the waveform than anything else here can.
//
// **What this rig does not measure is when the pixels become readable.** That
// is the optical half of the question and it needs a human, a camera or a
// photodiode; it is deliberately not here. This half is the one with levers
// attached to it -- what is paid before the waveform starts, and what is paid
// after it has finished.
//
// The three shapes are the firmware's three transitions, drawn with the same
// faces and the same geometry as src/sticky/screen.cpp so the row counts match:
// a full refresh (listening()), a partial over the word band (working()) and a
// partial over the whole panel (answer()). The text differs, which moves the
// cpu column and nothing else.
//
// One boot is one run: a full refresh has to precede the partials that are
// differential against it, which is exactly how a question is shaped. The
// pacing is E1's -- the sequence runs, the table prints, and the board sleeps
// with the AI button as the only way back, because it has no off switch.

#include <Arduino.h>
#include <Seeed_GFX.h>
#include <driver/gpio.h>
#include <esp_timer.h>

#include <font/GFXFF/FreeSans24pt7b.h>
#include <font/GFXFF/FreeSansBold24pt7b.h>

#include "sticky/buzzer.h"
#include "sticky/epaper.h"
#include "sticky/power.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// 0x22, Display Update Control 2: a list of steps 0x20 then runs. The library
// sends all of them in one go, which is why its busy wait covers the waveform
// and the power-down together.
constexpr uint8_t kFullBoth = 0xF7;     // what Driver_SSD1677::update() sends
constexpr uint8_t kFullDrive = 0xF4;    // ... without "disable analog, disable OSC"
constexpr uint8_t kPartialBoth = 0xFF;  // what updatePartial() sends
constexpr uint8_t kPartialDrive = 0xFC;
constexpr uint8_t kPowerDown = 0x03;    // the two steps taken out of the above

// Six passes over the three shapes, alternating unsplit and split, which is
// three of each per shape. About 26 seconds of panel.
constexpr uint32_t kPasses = 6;

// The band working() repaints, from the same arithmetic screen.cpp uses.
constexpr int32_t kWordBandMargin = 12;

constexpr uint32_t kMaxEdges = 64;
constexpr uint32_t kMaxRows = kPasses * 3;

enum Shape : uint8_t { kFull = 0, kBand = 1, kWhole = 2 };

const char* shapeName(uint8_t shape) {
  switch (shape) {
    case kFull: return "full";
    case kBand: return "band";
    default: return "whole";
  }
}

// Every timestamp of one refresh, in microseconds on esp_timer's clock. Filled
// by the driver's overrides and by the rig around the call itself.
struct Marks {
  int64_t begin, wake0, wake1, drive0, drive1, power0, power1;
  int64_t sleep0, sleep1, end;
  // Accumulated rather than spanned: between the two planes of a full refresh
  // Panel_EPaper flips the second one, and that is frame-buffer work, not bus
  // time. Summing the calls leaves the flip in the cpu residual where it
  // belongs.
  uint32_t pushUs;
  uint32_t pushBytes;
};

Marks g_marks;
bool g_split = false;

uint32_t us(int64_t from, int64_t to) {
  return to > from ? static_cast<uint32_t>(to - from) : 0;
}

// BUSY, both edges, timestamped from the interrupt rather than from a 1 ms
// poll. Armed once the panel is up and reset at the top of every refresh.
struct Edge {
  int64_t us;
  uint8_t level;
};

volatile Edge g_edges[kMaxEdges];
volatile uint32_t g_edgeCount = 0;
int8_t g_busyPin = -1;

void IRAM_ATTR busyIsr() {
  const uint32_t n = g_edgeCount;
  if (n >= kMaxEdges) return;
  g_edges[n].us = esp_timer_get_time();
  g_edges[n].level = static_cast<uint8_t>(gpio_get_level(static_cast<gpio_num_t>(g_busyPin)));
  g_edgeCount = n + 1;
}

// The firmware's driver, with the corrections in src/sticky/epaper.h intact and
// a timestamp around each step. Nothing here changes what reaches the
// controller except the one thing the rig is for: sending 0x22 twice instead of
// once, so the waveform and the power-down land in separate busy waits.
class Driver_SSD1677_Timed : public Driver_SSD1677_Sticky {
 public:
  Driver_SSD1677_Timed(uint16_t w = 800, uint16_t h = 480, int8_t busyPin = -1)
      : Driver_SSD1677_Sticky(w, h, busyPin) {}

  const char* name() const override { return "SSD1677 (inverted, timed)"; }

  void setBusyPin(int pin) override {
    g_busyPin = static_cast<int8_t>(pin);
    Driver_SSD1677_Sticky::setBusyPin(pin);
  }

  void wake() override {
    g_marks.wake0 = esp_timer_get_time();
    Driver_SSD1677_Sticky::wake();
    g_marks.wake1 = esp_timer_get_time();
  }

  void wakePartial() override {
    g_marks.wake0 = esp_timer_get_time();
    Driver_SSD1677_Sticky::wakePartial();
    g_marks.wake1 = esp_timer_get_time();
  }

  // The first plane of a full refresh. A partial never gets here: its previous
  // plane goes out from inside pushNewColors(), which is where the shadow lives.
  void pushOldColors(const uint8_t* data, size_t len) override {
    const int64_t t0 = esp_timer_get_time();
    Driver_SSD1677_Sticky::pushOldColors(data, len);
    g_marks.pushUs += us(t0, esp_timer_get_time());
    g_marks.pushBytes += static_cast<uint32_t>(len);
  }

  void pushNewColors(const uint8_t* data, size_t len) override {
    const int64_t t0 = esp_timer_get_time();
    Driver_SSD1677_Sticky::pushNewColors(data, len);
    g_marks.pushUs += us(t0, esp_timer_get_time());
    g_marks.pushBytes += static_cast<uint32_t>(len);
  }

  void update() override { run(kFullBoth, kFullDrive); }
  void updatePartial() override { run(kPartialBoth, kPartialDrive); }

  void sleep() override {
    g_marks.sleep0 = esp_timer_get_time();
    Driver_SSD1677_Sticky::sleep();
    g_marks.sleep1 = esp_timer_get_time();
  }

 private:
  // Unsplit is byte for byte what Driver_SSD1677::update() does. Split issues
  // the same steps in two goes, each with its own busy wait, so the waveform
  // and the shutdown after it are two numbers instead of one.
  void run(uint8_t both, uint8_t drive) {
    g_marks.drive0 = esp_timer_get_time();
    sequence(g_split ? drive : both);
    g_marks.drive1 = esp_timer_get_time();

    if (!g_split) return;

    g_marks.power0 = esp_timer_get_time();
    sequence(kPowerDown);
    g_marks.power1 = esp_timer_get_time();
  }

  void sequence(uint8_t byte) {
    if (!_bus) return;
    _bus->writeCommand(0x22);
    _bus->writeData(byte);
    _bus->writeCommand(0x20);
    if (g_busyPin >= 0) (void)waitForReadyPin(g_busyPin, false);
  }
};

// Same geometry as the firmware's Config_Sticky_SSD1677_Fixed; only the driver
// differs, and only by the timestamps.
struct Config_Sticky_SSD1677_Timed {
  using Driver = Driver_SSD1677_Timed;
  using Panel = Panel_EPaper;
  static constexpr uint16_t width = 800;
  static constexpr uint16_t height = 480;
  static constexpr uint8_t colorDepth = 1;
};

// One refresh, reduced to the numbers the table prints. Microseconds
// throughout: the phases at either end are tens of microseconds and rounding
// them to milliseconds would lose the point of measuring them.
struct Row {
  uint8_t shape;
  bool split;
  bool cold;  // the first refresh after the panel rail came up
  uint32_t cpuUs, wakeUs, pushUs, driveUs, powerUs, sleepUs, tailUs, totalUs;
  uint32_t pushBytes;
  uint32_t edges;
};

Row g_rows[kMaxRows];
uint32_t g_rowCount = 0;

Seeed_GFX display;

void armRefresh() {
  g_marks = Marks{};
  g_marks.begin = esp_timer_get_time();
  g_edgeCount = 0;
}

Row closeRefresh(uint8_t shape, bool cold) {
  g_marks.end = esp_timer_get_time();

  Row row = {};
  row.shape = shape;
  row.split = g_split;
  row.cold = cold;
  row.wakeUs = us(g_marks.wake0, g_marks.wake1);
  row.pushUs = g_marks.pushUs;
  row.driveUs = us(g_marks.drive0, g_marks.drive1);
  row.powerUs = us(g_marks.power0, g_marks.power1);
  row.sleepUs = us(g_marks.sleep0, g_marks.sleep1);
  row.tailUs = us(g_marks.sleep1, g_marks.end);
  row.totalUs = us(g_marks.begin, g_marks.end);
  row.pushBytes = g_marks.pushBytes;
  row.edges = g_edgeCount;

  // Everything the panel spent on the frame buffer rather than on the
  // controller: the residual, which is the only honest way to get it when the
  // work is spread over half a dozen private methods.
  const uint32_t accounted =
      row.wakeUs + row.pushUs + row.driveUs + row.powerUs + row.sleepUs + row.tailUs;
  row.cpuUs = row.totalUs > accounted ? row.totalUs - accounted : 0;

  if (g_rowCount < kMaxRows) g_rows[g_rowCount] = row;
  ++g_rowCount;
  return row;
}

void printEdges(const char* label) {
  const uint32_t count = g_edgeCount;
  Serial1.printf("  BUSY during the first %s refresh: %lu edges", label,
                 static_cast<unsigned long>(count));
  if (count == 0) {
    Serial1.println(" -- none, so the pin never moved while the interrupt was armed");
    return;
  }
  Serial1.println();
  for (uint32_t i = 0; i < count && i < kMaxEdges; ++i) {
    Serial1.printf("    %+8ld ms  %s\n",
                   static_cast<long>((g_edges[i].us - g_marks.drive0) / 1000),
                   g_edges[i].level ? "high" : "low");
  }
}

// The three screens, drawn the way src/sticky/screen.cpp draws them: the same
// faces, the same band, the same datums. Only the words differ.
void startWord() {
  display.setTextColor(TFT_BLACK);
  display.setFreeFont(&FreeSansBold24pt7b);
  display.setTextSize(2);
  display.setTextDatum(MC_DATUM);
}

void wordBand(int32_t& y, int32_t& height) {
  height = display.fontHeight() + 2 * kWordBandMargin;
  y = (display.height() - height) / 2;
}

void drawFull(uint32_t pass) {
  display.fillScreen(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextSize(1);
  display.setTextDatum(TL_DATUM);
  startWord();
  char word[16];
  snprintf(word, sizeof(word), "E8 %lu", static_cast<unsigned long>(pass));
  display.drawString(word, display.width() / 2, display.height() / 2);
}

void drawBand() {
  startWord();
  int32_t y = 0, height = 0;
  wordBand(y, height);
  display.fillRect(0, y, display.width(), height, TFT_WHITE);
  display.drawString("WORKING", display.width() / 2, display.height() / 2);
}

void drawWhole(uint32_t pass) {
  display.fillScreen(TFT_WHITE);
  display.setTextColor(TFT_BLACK);
  display.setTextSize(1);
  display.setFreeFont(&FreeSans24pt7b);
  display.setTextDatum(ML_DATUM);
  char line[48];
  snprintf(line, sizeof(line), "pass %lu, %s", static_cast<unsigned long>(pass),
           g_split ? "split" : "one go");
  display.drawString(line, 40, display.height() / 2);
}

constexpr const char* kHeader =
    "  %-3s %-6s %-7s %8s %8s %8s %8s %8s %8s %8s %9s\n";
constexpr const char* kRowFormat =
    "  %-3s %-6s %-7s %8s %8s %8s %8s %8s %8s %8s %9s\n";

void field(char* out, size_t size, uint32_t microseconds) {
  if (microseconds == 0) {
    snprintf(out, size, "--");
  } else if (microseconds < 10000) {
    snprintf(out, size, "%lu us", static_cast<unsigned long>(microseconds));
  } else {
    snprintf(out, size, "%lu ms", static_cast<unsigned long>(microseconds / 1000));
  }
}

void printTable() {
  Serial1.println();
  Serial1.printf(kHeader, "#", "shape", "0x22", "cpu", "wake", "push", "drive", "power", "sleep",
                 "tail", "total");

  const uint32_t rows = g_rowCount < kMaxRows ? g_rowCount : kMaxRows;
  for (uint32_t i = 0; i < rows; ++i) {
    const Row& row = g_rows[i];
    char index[8], cpu[12], wake[12], push[12], drive[12], power[12], sleep[12], tail[12],
        total[12];

    snprintf(index, sizeof(index), "%lu", static_cast<unsigned long>(i + 1));
    field(cpu, sizeof(cpu), row.cpuUs);
    field(wake, sizeof(wake), row.wakeUs);
    field(push, sizeof(push), row.pushUs);
    field(drive, sizeof(drive), row.driveUs);
    field(power, sizeof(power), row.powerUs);
    field(sleep, sizeof(sleep), row.sleepUs);
    field(tail, sizeof(tail), row.tailUs);
    field(total, sizeof(total), row.totalUs);

    Serial1.printf(kRowFormat, index, shapeName(row.shape), row.split ? "split" : "one go", cpu,
                   wake, push, drive, power, sleep, tail, total);
  }
  Serial1.println();
}

// The median of one column over the rows of one shape and one mode. Three
// samples each, so this is the middle one and it is only worth more than the
// mean because a cold first refresh should not drag the number it is not about.
uint32_t median(uint8_t shape, bool split, uint32_t Row::*field) {
  uint32_t values[kMaxRows];
  uint32_t n = 0;
  const uint32_t rows = g_rowCount < kMaxRows ? g_rowCount : kMaxRows;
  for (uint32_t i = 0; i < rows; ++i) {
    if (g_rows[i].shape == shape && g_rows[i].split == split) values[n++] = g_rows[i].*field;
  }
  if (n == 0) return 0;
  for (uint32_t i = 1; i < n; ++i) {
    const uint32_t v = values[i];
    uint32_t j = i;
    while (j > 0 && values[j - 1] > v) {
      values[j] = values[j - 1];
      --j;
    }
    values[j] = v;
  }
  return values[n / 2];
}

void printSummary() {
  Serial1.println("  medians, in milliseconds, and what each shape spends where:");
  Serial1.println();

  for (uint8_t shape = kFull; shape <= kWhole; ++shape) {
    const uint32_t one = median(shape, false, &Row::totalUs);
    const uint32_t split = median(shape, true, &Row::totalUs);
    const uint32_t drive = median(shape, true, &Row::driveUs);
    const uint32_t power = median(shape, true, &Row::powerUs);
    const uint32_t cpu = median(shape, true, &Row::cpuUs);
    const uint32_t wake = median(shape, true, &Row::wakeUs);
    const uint32_t push = median(shape, true, &Row::pushUs);
    const uint32_t sleep = median(shape, true, &Row::sleepUs);
    const uint32_t tail = median(shape, true, &Row::tailUs);

    Serial1.printf("  %-5s  total %lu ms in one go, %lu ms split\n", shapeName(shape),
                   static_cast<unsigned long>(one / 1000),
                   static_cast<unsigned long>(split / 1000));
    Serial1.printf("         waveform %lu ms, power-down %lu ms\n",
                   static_cast<unsigned long>(drive / 1000),
                   static_cast<unsigned long>(power / 1000));
    Serial1.printf("         before it: %lu ms cpu, %lu ms wake, %lu ms push\n",
                   static_cast<unsigned long>(cpu / 1000), static_cast<unsigned long>(wake / 1000),
                   static_cast<unsigned long>(push / 1000));
    Serial1.printf("         after it: %lu ms driver sleep, %lu ms panel tail\n",
                   static_cast<unsigned long>(sleep / 1000),
                   static_cast<unsigned long>(tail / 1000));

    // The whole point of alternating: if taking the power-down steps out of
    // 0x22 changed the total, the byte does not mean what the split assumes and
    // the power column is not a measurement.
    const long delta =
        static_cast<long>(split / 1000) - static_cast<long>(one / 1000);
    Serial1.printf("         split costs %+ld ms against one go -- %s\n", delta,
                   (delta > -40 && delta < 40)
                       ? "the same refresh, so the split is honest"
                       : "NOT the same refresh: distrust the power column");
    Serial1.println();
  }
}

void runShape(uint8_t shape, uint32_t pass, bool cold) {
  switch (shape) {
    case kFull: drawFull(pass); break;
    case kBand: drawBand(); break;
    default: drawWhole(pass); break;
  }

  armRefresh();
  bool ok = true;
  if (shape == kFull) {
    display.refresh();
  } else if (shape == kBand) {
    int32_t y = 0, height = 0;
    startWord();
    wordBand(y, height);
    ok = display.refreshPartial(0, y, display.width(), height).ok();
  } else {
    ok = display.refreshPartial(0, 0, display.width(), display.height()).ok();
  }
  const Row row = closeRefresh(shape, cold);

  Serial1.printf("  pass %lu %-5s %-6s: %lu ms%s\n", static_cast<unsigned long>(pass),
                 shapeName(shape), g_split ? "split" : "one go",
                 static_cast<unsigned long>(row.totalUs / 1000),
                 ok ? "" : "  -- the controller refused the partial update");

  if (pass == 1 && shape != kWhole) printEdges(shapeName(shape));
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  stickyPower::holdLatch();
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  Serial1.println();
  Serial1.printf("E8 refresh-phase rig -- wake %s, %lu passes over three shapes\n",
                 stickyPower::wakeupCauseName(), static_cast<unsigned long>(kPasses));

  if (!display.begin<Board_reTerminal_Sticky, Config_Sticky_SSD1677_Timed>()) {
    Serial1.printf("FAILED: the panel would not start: %s\n", display.lastResult().message);
    Serial1.flush();
    stickyBuzzer::error();
    stickyPower::deepSleep();
  }

  Serial1.printf("  panel up %lu ms after entry, BUSY on GPIO%d\n",
                 static_cast<unsigned long>((esp_timer_get_time() - tEntry) / 1000), g_busyPin);

  if (g_busyPin >= 0) attachInterrupt(g_busyPin, busyIsr, CHANGE);

  for (uint32_t pass = 1; pass <= kPasses; ++pass) {
    // Alternating rather than blocked, so a drift in the panel over half a
    // minute cannot be read as a difference between the two modes.
    g_split = (pass % 2) == 0;
    for (uint8_t shape = kFull; shape <= kWhole; ++shape) {
      runShape(shape, pass, pass == 1 && shape == kFull);
    }
  }

  if (g_busyPin >= 0) detachInterrupt(g_busyPin);

  printTable();
  printSummary();

  stickyBuzzer::answer();

  Serial1.println("  press the AI button for another run");
  Serial1.flush();
  stickyPower::deepSleep();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
