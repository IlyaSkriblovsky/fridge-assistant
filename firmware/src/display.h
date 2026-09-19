#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <stdint.h>

#include "sticky/screen.h"

// The panel, on a thread of its own.
//
// A refresh is 0.8 to 2.3 s of the panel's own timeline, and none of it needs
// the orchestrator's thread, which has a release to act on and an upload to
// feed. So the orchestrator posts a screen and carries on, and this task draws
// it.
//
// **It wraps StickyScreen** and owns the only instance of it, so nothing outside
// this class can reach the panel at all.
//
// **A slot of one, with replacement.** A screen posted while another is still
// waiting for the panel replaces it; the refresh already on the panel is never
// cut short. A screen that was stale before the panel got to it is never drawn:
// on a short hold with a quick backend the answer lands while LISTENING is
// still refreshing, and drawing WORKING first would cost 889 ms to show a word
// that stopped being true before it appeared. The pre-clear is the one post
// that is never replaced, because it is not a screen but the controller being
// reconciled with the glass: it runs ahead of whatever is posted after it.
//
// **The task wants no priority to speak of**, because a refresh is almost all
// waiting: the library polls BUSY with delay(1), which is vTaskDelay() against
// a 1000 Hz tick, so the task is blocked for nearly the whole of it. What it
// does spend CPU on is the drawing before a refresh -- about 260 ms for a full
// screen -- and the bit reversal of the plane push inside one, 222 ms (E8).
//
// Nothing here prints; Serial1 is the orchestrator's. What the panel did is
// kept per screen, on this task's clock, and read back once the task is idle.
class Display {
 public:
  // Core 0, beside the WiFi and lwIP tasks, which outrank it by a mile and
  // sleep most of the time. Core 1 is capture's, and the orchestrator's: the
  // orchestrator streams the upload while the button is held, and the start of
  // that stream is exactly when this task is drawing LISTENING and
  // pushing its planes -- half a second of CPU that would otherwise be
  // time-sliced against the stream at the same priority.
  //
  // At priority 1 the one thing on core 0 it can hold up is the idle task, which
  // the task watchdog watches on that core with a 5 s timeout. The longest this
  // task runs without blocking is one screen's drawing followed by its plane
  // push, about half a second.
  static constexpr BaseType_t kCore = 0;
  static constexpr UBaseType_t kPriority = 1;

  // The library's drawing and refresh paths, the FreeFont glyph code, and two
  // 128-byte strings in StickyScreen::error(). 1.8 to 2.0 KB of it is in use
  // (S10), and the log keeps reporting it; the rest stays because the library's
  // fallback paths -- a refused partial, a BUSY timeout -- have never run here,
  // and a task that overflows takes the question with it.
  static constexpr uint32_t kStackBytes = 8192;

  // Every screen, and the pre-clear -- which is the record's index as well.
  enum class Screen : uint8_t { Clear, Listening, Working, Answer, Error, Count };

  // What became of one screen. Times are esp_timer_get_time(), so they share an
  // axis with everything the orchestrator measures.
  struct Record {
    int64_t postedUs = 0;     // when it was posted; zero if it never was
    int64_t startUs = 0;      // when the task took it; zero if it never did
    int64_t endUs = 0;        // when its refresh returned
    bool superseded = false;  // replaced while it waited, and never drawn
    bool partial = false;     // StickyScreen::lastWasPartial() after it
    const char* error = "";   // why the controller refused a partial, if it did
  };

  ~Display();

  // Brings the panel up and starts the task. StickyScreen::begin() runs here,
  // on the caller's thread, before the task exists, which keeps the vision's
  // one failure with nothing to draw on -- chirp, log, sleep -- a return value
  // rather than something that arrives later in a message.
  //
  // It is 201 ms on every wake, which is more than it looks like it should be
  // and has not been taken apart: E8 timed the controller's re-initialisation
  // inside a refresh at 24 ms. Doing it here costs nothing the user waits for.
  // LISTENING cannot start before it on any thread, and the orchestrator has
  // nothing to do until the release but watch.
  //
  // Returns true at once if the task is already running, so every path to a
  // screen can call it. False leaves nothing running; lastError() says why.
  bool start();
  bool running() const { return _task != nullptr; }

  // The screens, posted. Each returns at once; the task draws it when the panel
  // is free, unless something posted after it gets there first. Posting before
  // start() has succeeded does nothing.
  //
  // clear() is the cold-start pre-clear and is never superseded: it runs ahead
  // of whatever is posted next. See StickyScreen::clear().
  void clear();
  void listening();
  void working();
  void answer(const char* text);
  void error(const char* title, const char* detail = nullptr);

  // Blocks until nothing is waiting and the panel is not refreshing, up to
  // timeoutMs. False on the timeout, which means the task is still inside a
  // refresh. Returns true at once if the task never started.
  //
  // The exit needs this: deep sleep takes the panel's rail with it, and a
  // refresh cut in half leaves the glass half-driven.
  bool waitIdle(uint32_t timeoutMs);

  // What the panel did with each screen. Only meaningful once waitIdle() has
  // returned true: until then the task may still be writing it.
  const Record& record(Screen screen) const {
    return _records[static_cast<uint8_t>(screen)];
  }

  // The least free stack the task has had, in bytes. For the log, to check
  // kStackBytes against.
  uint32_t stackUnusedBytes() const;

  const char* lastError() const { return _lastError; }

 private:
  // What is waiting for the panel: at most one screen, and the text it carries,
  // already made drawable.
  struct Slot {
    Screen screen = Screen::Count;  // Count: nothing waiting
    char text[StickyScreen::kMaxTextChars] = {0};
    char detail[StickyScreen::kMaxTextChars] = {0};
  };

  static void trampoline(void* self);
  void run();
  void post(Screen screen, const char* text, const char* detail);
  void draw(const Slot& slot);

  StickyScreen _screen;

  TaskHandle_t _task = nullptr;
  SemaphoreHandle_t _lock = nullptr;  // guards the slot, the flag and the idle bit
  EventGroupHandle_t _events = nullptr;

  Slot _slot;
  bool _clearPending = false;

  Record _records[static_cast<uint8_t>(Screen::Count)];
  const char* _lastError = "";
};
