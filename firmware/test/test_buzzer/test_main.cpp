#include <unity.h>
#include <vector>
#include "rtttl_validation.h"
// Compile the production buzzer and actual library's ESP32 branch; only the
// hardware and clock are fakes. No Arduino tone()/noTone() implementation.
#define ESP32 1
#include "../../src/sticky/buzzer.cpp"

struct Tone { uint32_t hz; unsigned long at; };
std::vector<Tone> tones;
unsigned long clockMs;
bool muted, attachOk, attached, parked;
unsigned long millis() { return clockMs; }
void delay(unsigned long ms) { clockMs += ms; }
bool ledcAttach(uint8_t pin, uint32_t, uint8_t bits) {
  TEST_ASSERT_EQUAL(48, pin);
  TEST_ASSERT_EQUAL(10, bits);
  attached = attachOk;
  return attachOk;
}
bool ledcDetach(uint8_t pin) {
  TEST_ASSERT_EQUAL(48, pin);
  attached = false;
  return true;
}
uint32_t ledcWriteTone(uint8_t pin, uint32_t hz) {
  TEST_ASSERT_EQUAL(48, pin); // Catch the upstream channel-0 pause regression.
  TEST_ASSERT_TRUE(attached);
  tones.push_back({hz, clockMs});
  return hz;
}
void pinMode(uint8_t pin, uint8_t mode) {
  TEST_ASSERT_EQUAL(48, pin);
  TEST_ASSERT_EQUAL(OUTPUT, mode);
}
void digitalWrite(uint8_t pin, uint8_t level) {
  TEST_ASSERT_EQUAL(48, pin);
  TEST_ASSERT_EQUAL(LOW, level);
  parked = true;
}
bool silentMode::enabled() { return muted; }
void setUp() {
  tones.clear(); clockMs = 100; muted = false; attachOk = true;
  attached = false; parked = false;
}
void tearDown() {}
constexpr const char* kEriccson =
  "Ericcson:d=4,o=5,b=355:16b4,16d,16b4,16d,16b4,16d,16b4,16d,"
  "16d,16f,16d,16f,16d,16f,16d,16f,16f,16a,16f,16a,16f,16a,16f,16a,1p,2p,"
  "16b4,16d,16b4,16d,16b4,16d,16b4,16d,16d,16f,16d,16f,16d,16f,16d,16f,"
  "16f,16a,16f,16a,16f,16a,16f,16a";
void playEriccson() {
  TEST_ASSERT_TRUE(stickyBuzzer::startRtttl(kEriccson));
  while (stickyBuzzer::pollRtttl()) delay(1);
}
void ericcson_notes_rests_and_completion() {
  muted = true; // Reminder playback bypasses silent mode.
  playEriccson();
  TEST_ASSERT_EQUAL(51, tones.size()); // 48 notes, two rests, final stop.
  const uint32_t pairs[][2] = {{493, 587}, {587, 698}, {698, 880}};
  for (int phrase = 0; phrase < 2; ++phrase) {
    for (int i = 0; i < 24; ++i) {
      const int index = phrase * 26 + i;
      TEST_ASSERT_EQUAL(pairs[i / 8][i % 2], tones[index].hz);
      TEST_ASSERT_EQUAL(100 + phrase * 2022 + i * 42, tones[index].at);
    }
  }
  TEST_ASSERT_EQUAL(0, tones[24].hz);
  TEST_ASSERT_EQUAL(1108, tones[24].at);
  TEST_ASSERT_EQUAL(0, tones[25].hz);
  TEST_ASSERT_EQUAL(1784, tones[25].at);
  TEST_ASSERT_EQUAL(0, tones.back().hz);
  TEST_ASSERT_EQUAL(3130, clockMs);
  TEST_ASSERT_FALSE(attached);
  TEST_ASSERT_TRUE(parked);
  // A second reminder must start the full ringtone again.
  tones.clear();
  playEriccson();
  TEST_ASSERT_EQUAL(51, tones.size());
  TEST_ASSERT_EQUAL(6160, clockMs);
}
void voice_cues_and_attachment_failure() {
  muted = true;
  stickyBuzzer::ready(); stickyBuzzer::taken();
  stickyBuzzer::answer(); stickyBuzzer::error();
  TEST_ASSERT_TRUE(tones.empty());
  muted = false;
  stickyBuzzer::taken();
  TEST_ASSERT_EQUAL(1, tones.size());
  TEST_ASSERT_EQUAL(2500, tones[0].hz);
  TEST_ASSERT_EQUAL(160, clockMs);
  TEST_ASSERT_TRUE(parked);
  tones.clear(); attachOk = false;
  stickyBuzzer::notification();
  TEST_ASSERT_TRUE(tones.empty());
  TEST_ASSERT_EQUAL(160, clockMs);
}
void responsive_player_and_validation() {
  TEST_ASSERT_TRUE(rtttl::valid(config::kNotificationRtttl));
  muted = true;
  stickyBuzzer::notification();
  TEST_ASSERT_FALSE(tones.empty());
  TEST_ASSERT_FALSE(attached);
  TEST_ASSERT_TRUE(parked);
  tones.clear(); clockMs = 100;
  TEST_ASSERT_TRUE(rtttl::valid("Test:b=120,o=5,d=8:c#,d.6,p"));
  const char* bad[] = {"", "NoColon", "X:", "X:d=0:c", "X:b=0:c", "X:o=9:c", "X:o=01:c",
    "X:d=4,d=8:c", "X:d=4:16", "X:d=4:c,", "X:d=4:x", "X:d=4:c..",
    "X:d=4:p#", "X:l=0:c", "X:b=25:1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c,1c"};
  for (auto song : bad) TEST_ASSERT_FALSE(rtttl::valid(song));
  TEST_ASSERT_FALSE(rtttl::valid("X:o=7:c", 1));
  TEST_ASSERT_FALSE(rtttl::valid("X:o=0:c", -1));
  TEST_ASSERT_TRUE(stickyBuzzer::startRtttl("X:d=4,o=5,b=120:a", 1));
  TEST_ASSERT_EQUAL(1760, tones.back().hz);
  TEST_ASSERT_TRUE(stickyBuzzer::pollRtttl());
  stickyBuzzer::stopRtttl(); // Stop during the note, before its 500 ms deadline.
  TEST_ASSERT_EQUAL(100, clockMs);
  TEST_ASSERT_EQUAL(0, tones.back().hz);
  TEST_ASSERT_FALSE(attached);
  TEST_ASSERT_TRUE(parked);
  TEST_ASSERT_FALSE(stickyBuzzer::pollRtttl());
  TEST_ASSERT_TRUE(stickyBuzzer::startRtttl("X:d=4,o=5,b=120:a", -1));
  TEST_ASSERT_EQUAL(440, tones.back().hz);
  stickyBuzzer::stopRtttl();
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(responsive_player_and_validation);
  RUN_TEST(ericcson_notes_rests_and_completion);
  RUN_TEST(voice_cues_and_attachment_failure);
  return UNITY_END();
}
