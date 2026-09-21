#include "sticky/buzzer.h"

#include <Arduino.h>
#include "silent_mode.h"

namespace {

// ledcWriteTone() reconfigures the timer to 10 bits and writes a 50% duty
// whatever the attach asked for, so the attach may as well ask for the same.
constexpr uint8_t kResolutionBits = 10;

// Frequencies and durations are a choice, not a measurement -- the only
// requirement is that the four are distinguishable without looking. The two
// pairs share their notes and differ in direction: the rising pair opens a
// question, the falling pair closes it. The error note is neither, and is lower
// and longer than anything in the pairs so it cannot be mistaken for one that
// was cut short.
//
// The taken note has to survive being heard half a second before the answer's
// pair, which is what E7 says the usual round trip is, so it is placed where
// neither pair can absorb it: one note rather than two, between the pair's two
// pitches so it is neither of them, and short enough that it cannot be taken
// for the error's sustained note.
//
// Kept near 2-3 kHz, where a small piezo is loudest.
constexpr uint16_t kLowHz = 2000;
constexpr uint16_t kMidHz = 2500;
constexpr uint16_t kHighHz = 3000;

constexpr stickyBuzzer::Note kReady[] = {{kLowHz, 40}, {0, 30}, {kHighHz, 40}};
constexpr stickyBuzzer::Note kTaken[] = {{kMidHz, 60}};
constexpr stickyBuzzer::Note kAnswer[] = {{kHighHz, 90}, {0, 50}, {kLowHz, 90}};
constexpr stickyBuzzer::Note kError[] = {{1500, 450}};

// Detached and driven low by hand rather than left as an idle LEDC output:
// prepareDeepSleep() parks GPIO48 low through the sleep, and gpio_hold_en()
// holds whatever level the pad is at when it runs.
void park() {
  ledcDetach(stickyBuzzer::kPin);
  pinMode(stickyBuzzer::kPin, OUTPUT);
  digitalWrite(stickyBuzzer::kPin, LOW);
}

}  // namespace

void stickyBuzzer::play(const Note* notes, size_t count) {
  if (silentMode::enabled() || notes == nullptr || count == 0) return;

  // Attached once for the whole pattern; re-attaching between notes would put a
  // pad glitch into every gap. The attach frequency is irrelevant -- the first
  // ledcWriteTone() below sets the one that is heard.
  if (!ledcAttach(kPin, kHighHz, kResolutionBits)) return;

  for (size_t i = 0; i < count; ++i) {
    ledcWriteTone(kPin, notes[i].hz);  // hz 0 writes a zero duty: a rest
    delay(notes[i].ms);
  }

  park();
}

void stickyBuzzer::ready() { play(kReady); }

void stickyBuzzer::taken() { play(kTaken); }

void stickyBuzzer::answer() { play(kAnswer); }

void stickyBuzzer::error() { play(kError); }
