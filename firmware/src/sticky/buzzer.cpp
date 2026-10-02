#include "sticky/buzzer.h"

#include <Arduino.h>
#include "config.h"
#include "silent_mode.h"

static int rtttlOctaveShift = 0;
static bool rtttlActive = false;

// PlayRtttl 2.2.0 uses GPIO numbers for notes but the obsolete channel 0
// for rests on ESP32, and asynchronous noTone() for completion. Keep all
// library output on our already attached pin and stop it synchronously.
// Scope these substitutions to the pinned header-only library, never Arduino.
static void rtttlWriteTone(uint8_t, uint32_t hz) {
  if (hz) hz = rtttlOctaveShift >= 0 ? hz << rtttlOctaveShift : hz >> -rtttlOctaveShift;
  ledcWriteTone(stickyBuzzer::kPin, hz);
}
static void rtttlStopTone(uint8_t) {
  ledcWriteTone(stickyBuzzer::kPin, 0);
}
#define ledcWriteTone rtttlWriteTone
#define noTone rtttlStopTone
#include <PlayRtttl.hpp>
#undef noTone
#undef ledcWriteTone
#undef isdigit

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
// Legacy voice cues; E12 later preferred the 3.75-4.0 kHz region on this unit.
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

static void playNotes(const stickyBuzzer::Note* notes, size_t count) {
  using namespace stickyBuzzer;
  if (notes == nullptr || count == 0) return;

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

void stickyBuzzer::play(const Note* notes, size_t count) {
  if (!silentMode::enabled()) playNotes(notes, count);
}
// Reminder alarms bypass silent mode; ordinary voice cues still use play().
bool stickyBuzzer::startRtttl(const char* song, int octaveShift) {
  stopRtttl();
  if (!ledcAttach(kPin, kHighHz, kResolutionBits)) return false;
  rtttlOctaveShift = octaveShift;
  rtttlActive = true;
  startPlayRtttl(kPin, song);
  return true;
}
bool stickyBuzzer::pollRtttl() {
  if (!rtttlActive) return false;
  if (updatePlayRtttl()) return true;
  rtttlActive = false;
  park();
  return false;
}
void stickyBuzzer::stopRtttl() {
  if (!rtttlActive) return;
  stopPlayRtttl();
  rtttlActive = false;
  park();
}
void stickyBuzzer::notification() {
  if (!startRtttl(config::kNotificationRtttl)) return;
  while (pollRtttl()) delay(1);
}
