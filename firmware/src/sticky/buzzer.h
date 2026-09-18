#pragma once

#include <stddef.h>
#include <stdint.h>

// reTerminal Sticky (E1005) buzzer on GPIO48.
//
// The only feedback fast enough to be useful: the e-paper is one to two seconds
// behind everything, so the buzzer is what tells the user when the microphone
// went live, that the question was taken, and how it ended. Four patterns,
// meant to be told apart without looking -- see the sound table in
// docs/project-vision.md.
//
// Two properties the callers depend on:
//
//  * Driven through LEDC, not tone(). tone() hands the note to a background
//    task and returns before the sound ends, so a chirp issued just before
//    esp_deep_sleep_start() would either be cut off or still sounding when the
//    pads are parked.
//  * Every call blocks until the last note is over and leaves the pad low and
//    detached. stickyPower::prepareDeepSleep() parks GPIO48 low for the
//    duration of the sleep, and that parking only holds what it finds.

namespace stickyBuzzer {

constexpr int kPin = 48;

// One step of a pattern: a tone at hz for ms, or a rest when hz is 0.
struct Note {
  uint16_t hz;
  uint16_t ms;
};

// Plays the notes in order and returns when the last one has finished. Does
// nothing if the LEDC channel cannot be attached -- a silent buzzer is not
// worth failing a question over.
void play(const Note* notes, size_t count);

template <size_t N>
inline void play(const Note (&notes)[N]) {
  play(notes, N);
}

// The four patterns from the vision's table. Durations below.
void ready();   // microphone is live: two very short notes, low then high
void taken();   // the question is on its way: one short note, between the two
void answer();  // the answer is on screen: two short notes, high then low
void error();   // one longer note, lower than either of the pairs

}  // namespace stickyBuzzer
