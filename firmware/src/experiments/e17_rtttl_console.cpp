// E17: interactive RTTTL audition over the CH343P USB/UART bridge.
#include <Arduino.h>
#include <string.h>
#include "sticky/power.h"
#include "sticky/buzzer.h"
#include "rtttl_validation.h"
#include "config.h"

// This minimal rig never loads the NVS preference. RTTTL playback bypasses it.
bool silentModeEnabled = false;
namespace silentMode { bool enabled() { return silentModeEnabled; } }
namespace {
char input[rtttl::kMaxBytes + 1] = {};
char song[rtttl::kMaxBytes + 1] = {};
size_t used = 0;
bool overflow = false, playing = false;
int shift = 0;
void stop() { stickyBuzzer::stopRtttl(); playing = false; }
void play() {
  if (!rtttl::valid(song, shift)) {
    Serial1.println("ERROR invalid RTTTL or octave out of range (0..7)"); return;
  }
  stop();
  playing = stickyBuzzer::startRtttl(song, shift);
  if (!playing) { Serial1.println("ERROR LEDC attachment failed"); return; }
  Serial1.printf("PLAY octave %+d: %s\n", shift, song);
}
void command() {
  if (!strcmp(input, ":stop")) { stop(); Serial1.println("STOP"); }
  else if (!strcmp(input, ":repeat")) play();
  else if (!strcmp(input, ":up") || !strcmp(input, ":down") || !strcmp(input, ":reset")) {
    const int next = !strcmp(input, ":reset") ? 0 : shift + (!strcmp(input, ":up") ? 1 : -1);
    if (!rtttl::valid(song, next)) { Serial1.println("ERROR octave shift outside supported range"); return; }
    shift = next; play();
  } else if (!strcmp(input, ":help")) {
    Serial1.println("RTTTL line plays; :repeat :stop :up :down :reset :help");
  } else if (input[0] == ':') Serial1.println("ERROR unknown command");
  else {
    if (!rtttl::valid(input)) { Serial1.println("ERROR invalid standard RTTTL (d/o/b, lowercase notes, no spaces)"); return; }
    stop(); // Library retains a pointer: don't replace the playing buffer first.
    strcpy(song, input); shift = 0; play();
  }
}
}
void setup() {
  stickyPower::holdLatch();
  Serial1.setRxBufferSize(4096);
  Serial1.begin(115200, SERIAL_8N1, 44, 43);
  strcpy(song, config::kNotificationRtttl);
  Serial1.println("E17 READY — RTTTL console, 115200 baud. :help for commands");
}
void loop() {
  // Bound processing so a stream of input cannot starve the note scheduler.
  for (unsigned n = 0; n < 256 && Serial1.available(); ++n) {
    const char c = Serial1.read();
    if (c == '\r') continue;
    if (c == '\n') {
      if (overflow) Serial1.println("ERROR line exceeds 2048 bytes; discarded");
      else if (used) { input[used] = 0; command(); }
      used = 0; overflow = false;
    } else if (c < 32 || c > 126) overflow = true;
    else if (!overflow && used < rtttl::kMaxBytes) input[used++] = c;
    else overflow = true;
  }
  if (playing && !stickyBuzzer::pollRtttl()) {
    playing = false; Serial1.println("DONE");
  }
  delay(1);
}
