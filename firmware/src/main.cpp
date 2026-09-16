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

// Long enough to be sure of the level, short enough not to be felt. The
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

// Waits for the button to be up and then pressed again, both debounced.
// Returns false if the idle timeout ran out first.
//
// The press that woke the board is still down when the first screen is drawn,
// which is why the release comes first: without it the walkthrough would run
// through all seven screens on one press.
bool waitForPress() {
  const int64_t deadlineUs = esp_timer_get_time() + kIdleTimeoutMs * 1000LL;

  bool wantDown = false;  // release first, then the press
  int64_t stableSinceUs = 0;

  while (esp_timer_get_time() < deadlineUs) {
    if (button.isDown() != wantDown) {
      stableSinceUs = 0;
    } else {
      const int64_t nowUs = esp_timer_get_time();
      if (stableSinceUs == 0) stableSinceUs = nowUs;
      if (nowUs - stableSinceUs >= kPressDebounceMs * 1000LL) {
        if (wantDown) return true;
        wantDown = true;
        stableSinceUs = 0;
      }
    }
    delay(5);
  }

  return false;
}

// Draws one step and says how long the panel took over it. The refresh is the
// whole cost of a screen -- the drawing into the frame buffer is memory writes
// and rounds to nothing next to it.
void drawStep(const Step& step) {
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

  Serial1.printf("     drawn in %lu ms\n",
                 static_cast<unsigned long>((esp_timer_get_time() - startUs) / 1000));
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(tEntry);

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

    drawStep(kSteps[i]);

    if (i + 1 == count) break;

    Serial1.println("     press the AI button for the next one");
    Serial1.flush();
    if (!waitForPress()) {
      Serial1.println("  nobody pressed anything for five minutes -- sleeping");
      break;
    }
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
