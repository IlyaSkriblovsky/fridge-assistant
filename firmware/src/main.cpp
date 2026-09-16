// S5 -- the screens. A temporary driver, the way S1's to S4's were:
// docs/implementation.md asks for all three screens driven before the flow
// exists, because this is the one step whose failure mode is cosmetic and only
// a human can see it. S8 replaces all of this with the orchestrator.
//
// So the driver is a walkthrough: one screen per press of the AI button, in an
// order that puts every question S5 has to answer on the panel.
//
//   1  LISTENING                     the word, at the size it is meant to be read
//   2  a short answer                the phrase the backend actually returns
//   3  a long answer                 D2: it runs off the right edge, on purpose
//   4  an answer with Cyrillic in it D1: '?' per character, not garbage
//   5  NO WIFI                       an error title on its own
//   6  SERVER ERROR, status 500      a title with the detail under it
//   7  NO MICROPHONE                 the widest title in the vision's table
//
// Seven full refreshes back to back is also a ghosting test the flow itself
// never performs: whatever the panel keeps of screen 6 is visible on screen 7.
//
// **The press is latched in hardware, not polled.** A full refresh blocks this
// task for two and a half seconds and the image is on the glass before the call
// returns -- the waveform is still finishing, and the library then spends
// 200 ms in delay() putting the controller to sleep (100 ms in
// Driver_SSD1677::sleep() and 100 ms more in Panel_EPaper::ePaperSleep()). A
// press that starts and ends inside that window never existed as far as a poll
// is concerned, which is exactly what someone pressing as soon as the screen
// appears does. So the falling edge sets a counter from an interrupt and the
// wait compares against a snapshot taken before the refresh.
//
// The flow has no such gap: from S4 the capture task polls the button every
// 16 ms right through the Listening refresh, which is the whole reason the two
// tasks are split.
//
// **The board has no off switch**, so nothing here repeats. The walkthrough
// ends in deep sleep, five idle minutes end it early, and the AI button starts it
// again -- the same reasoning as S1's listening test, which could otherwise
// only be stopped by pulling the battery.

#include <Arduino.h>
#include <esp_timer.h>

#include "sticky_button.h"
#include "sticky_buzzer.h"
#include "sticky_power.h"
#include "sticky_screen.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// Nobody is watching any more; go to sleep rather than sit on the battery.
// Long enough to walk away from the desk mid-screen and come back, which one
// minute was not: the walkthrough is paced by a human looking at a panel.
constexpr uint32_t kIdleTimeoutMs = 300000;

// How close together two falling edges have to be to be the same press. The
// walkthrough is not the flow -- the flow's 40 ms release debounce is
// config::kButtonDebounceMs and lives in StickyButton.
constexpr uint32_t kPressDebounceMs = 40;

StickyScreen screen;
StickyButton button;

enum class Kind : uint8_t { Listening, Answer, Error };

struct Step {
  Kind kind;
  const char* a;     // the answer's text, or the error's title
  const char* b;     // the error's detail
  const char* what;  // what this step is here to show, for the log
};

const Step kSteps[] = {
    {Kind::Listening, nullptr, nullptr, "the Listening screen"},
    {Kind::Answer, "Received 160044 bytes", nullptr, "the answer the backend really returns"},
    {Kind::Answer, "Sorry, I did not catch that -- could you say it again, closer?", nullptr,
     "an answer too long for the line: D2, it runs off the right edge"},
    {Kind::Answer, "Privet is \xD0\xBF\xD1\x80\xD0\xB8\xD0\xB2\xD0\xB5\xD1\x82 in Russian", nullptr,
     "non-ASCII: D1, one '?' per character and nothing drawn wrong"},
    {Kind::Error, "NO WIFI", nullptr, "an error title on its own"},
    {Kind::Error, "SERVER ERROR", "status 500", "an error title with its detail"},
    {Kind::Error, "NO MICROPHONE", nullptr, "the widest title in the vision's table"},
};

// Everything that goes wrong here ends the same way the vision says a display
// failure ends: there is nothing to draw the message on, so chirp, log, sleep.
[[noreturn]] void fail(const char* what, const char* detail) {
  Serial1.printf("FAILED: %s -- %s\n", what, detail);
  Serial1.flush();
  stickyBuzzer::error();
  stickyPower::deepSleep();
}

// Presses seen so far, counted from the interrupt so one cannot be missed
// while the panel has this task. Bounce is rejected on the timestamp: a real
// press is one falling edge, and a hold is still one.
volatile uint32_t pressCount = 0;
volatile int64_t lastEdgeUs = 0;

void ARDUINO_ISR_ATTR onButtonEdge() {
  const int64_t nowUs = esp_timer_get_time();
  if (nowUs - lastEdgeUs < kPressDebounceMs * 1000LL) return;
  lastEdgeUs = nowUs;
  ++pressCount;
}

// Waits until the counter moves past `seen`, which is read before the refresh
// starts, so a press made while the screen was appearing already counts.
// Returns false if the idle timeout ran out first.
//
// The press that woke the board never lands here: it was already down when
// setup() ran, so its falling edge happened before there was an interrupt
// handler to see it, and its release is a rising edge.
bool waitForPress(uint32_t seen) {
  const int64_t deadlineUs = esp_timer_get_time() + kIdleTimeoutMs * 1000LL;

  while (esp_timer_get_time() < deadlineUs) {
    if (pressCount != seen) return true;
    delay(5);
  }

  return false;
}

// Draws one step and says how long the panel took over it, returning the moment
// the panel handed this task back. The refresh is the whole cost of a screen --
// the drawing into the frame buffer is memory writes and rounds to nothing next
// to it.
int64_t drawStep(const Step& step) {
  const int64_t startUs = esp_timer_get_time();

  switch (step.kind) {
    case Kind::Listening:
      screen.listening();
      break;
    case Kind::Answer:
      screen.answer(step.a);
      break;
    case Kind::Error:
      screen.error(step.a, step.b);
      break;
  }

  const int64_t doneUs = esp_timer_get_time();
  Serial1.printf("     drawn in %lu ms\n",
                 static_cast<unsigned long>((doneUs - startUs) / 1000));
  return doneUs;
}

// The last falling edge, read without an interrupt landing in the middle of it.
// A 64-bit load is two instructions on this core, so a press arriving between
// them would hand back half of one timestamp and half of another.
int64_t lastEdgeAt() {
  noInterrupts();
  const int64_t us = lastEdgeUs;
  interrupts();
  return us;
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(tEntry);  // takes GPIO4 back from the RTC pad and pulls it up
  attachInterrupt(digitalPinToInterrupt(StickyButton::kPin), onButtonEdge, FALLING);

  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  Serial1.println();
  Serial1.println("S5 screen driver -- one screen per press, then back to sleep");
  Serial1.printf("  wake %s, reset %s\n", stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());

  if (!screen.begin()) fail("display", screen.lastError());

  // The pre-clear S8 will make conditional, exercised here in the case that
  // needs it: after a power-on the controller's previous-image RAM has nothing
  // to do with what is on the glass. On a wake the first screen overwrites the
  // panel anyway, so the second and a half is not spent.
  if (!stickyPower::wokeFromDeepSleep()) {
    Serial1.println("  cold start: clearing the panel first");
    const int64_t startUs = esp_timer_get_time();
    screen.clear();
    Serial1.printf("     cleared in %lu ms\n",
                   static_cast<unsigned long>((esp_timer_get_time() - startUs) / 1000));
  }

  // The chirp the flow makes at this point, so the walkthrough sounds like the
  // device it is testing.
  stickyBuzzer::ready();

  const size_t count = sizeof(kSteps) / sizeof(kSteps[0]);
  for (size_t i = 0; i < count; ++i) {
    Serial1.printf("  %u/%u  %s\n", static_cast<unsigned>(i + 1), static_cast<unsigned>(count),
                   kSteps[i].what);
    Serial1.flush();

    // Read before drawing, not after: the press that advances past this screen
    // is very often made while this screen is still being refreshed.
    const uint32_t seen = pressCount;
    const int64_t doneUs = drawStep(kSteps[i]);

    if (i + 1 == count) break;

    Serial1.println("     press the AI button for the next one");
    Serial1.flush();
    if (!waitForPress(seen)) {
      Serial1.println("  nobody pressed anything for five minutes -- sleeping");
      break;
    }

    // Where the press fell relative to the moment the panel gave this task
    // back. A negative number is the case the poll used to lose outright: the
    // screen was already readable, the refresh had not returned yet, and
    // nothing but the interrupt was in a position to notice.
    Serial1.printf("     pressed %ld ms after the refresh returned%s\n",
                   static_cast<long>((lastEdgeAt() - doneUs) / 1000),
                   lastEdgeAt() < doneUs ? " -- i.e. during it" : "");
  }

  // The sound the flow makes once the answer is on the panel, and the end of
  // the walkthrough. The last screen stays up: that is the point of e-paper.
  stickyBuzzer::answer();

  Serial1.println("  done -- the last screen stays on the panel. Press to run again.");
  Serial1.flush();
  stickyPower::deepSleep();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
