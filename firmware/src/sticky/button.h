#pragma once

#include <esp_timer.h>
#include <stdint.h>

#include "sticky/power.h"

// reTerminal Sticky (E1005) AI button: active low, internal pull-up, and the
// ext1 source that wakes the board for every question.
//
// Three things make it more than a digitalRead():
//
//  * The press being timed is the one that woke the board, so it is already
//    down when setup() runs and there is no edge left to wait for. This class
//    reads the level and times how long it stays there, starting from a moment
//    the caller supplies -- the earliest timestamp it has, which is the top of
//    setup().
//  * GPIO4 comes back from deep sleep as an RTC pad, which is what ext1 and its
//    pull-up need across the sleep. digitalRead() means nothing until
//    rtc_gpio_deinit() has handed the pad back to the GPIO matrix.
//  * Release has to be debounced, or a contact bounce ends the utterance
//    mid-sentence. config::kButtonDebounceMs of uninterrupted high ends the
//    press; a press shorter than config::kButtonMinHoldMs is a tap, which the
//    caller discards without contacting the backend.
//
// poll() never blocks: from S4 on it is called by the capture task between I2S
// reads, where anything that waits costs audio. One task owns the object at a
// time -- S4 hands it to the capture task rather than sharing it, so there is
// no locking here.
//
// **Every hold reads short**, by the boot time plus however long the contact
// takes to become a wake event: 57 ms and an unmeasured millisecond or so
// (docs/experiments.md, E1). Nothing on this path can do better -- a button
// wake carries no deadline to measure the boot against -- so the 300 ms minimum
// hold asks for roughly 360 ms of real press, and the first ~60 ms of every
// press is spent booting rather than being timed.
//
// The side buttons on GPIO5 and GPIO6 are not used.

class StickyButton {
 public:
  // The pad ext1 wakes on and the pad read here are the same one, so the number
  // lives in one place.
  static constexpr int kPin = stickyPower::kPinAiButton;

  // Takes the pad back from the RTC subsystem and starts timing a press that is
  // assumed to be already down. pressStartUs is an esp_timer_get_time() reading
  // from the top of setup(); the no-argument form starts from now, which is
  // right when the press did not wake the board.
  void begin(int64_t pressStartUs);
  void begin() { begin(esp_timer_get_time()); }

  // Non-blocking. Returns true once the release has been debounced -- the same
  // thing released() reports -- and keeps returning true afterwards, so a late
  // bounce cannot end the same press twice.
  bool poll();

  bool released() const { return _released; }

  // How long the button has been down, or how long it was down once poll() has
  // reported the release. The press ends when the line first goes high; the
  // debounce window that follows confirms it and is not part of the hold.
  uint32_t heldMs() const;

  // True while the press is too short to count as a question. Meaningful once
  // released() is true; before that it only says the press is not long enough
  // yet.
  bool isTap() const;

  // The raw level, with no debouncing -- true while the contact is closed.
  bool isDown() const;

 private:
  int64_t _pressStartUs = 0;
  int64_t _highSinceUs = 0;  // when the line first went high, 0 while it is low
  int64_t _heldUs = 0;
  bool _released = false;
};
