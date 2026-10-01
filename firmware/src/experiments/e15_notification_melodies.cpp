// E15: blind preference test for four-element notification melodies.
//
// Controls (all buttons are active low):
//   Up / GPIO5   choose the first complete melody (A)
//   AI / GPIO4   start the experiment, or replay the current pair
//   Down / GPIO6 choose the second complete melody (B)
//
// Do not watch the UART while choosing: it reveals pattern names after a vote.

#include <Arduino.h>
#include <esp_system.h>
#include <driver/rtc_io.h>
#include <math.h>

#include "sticky/buzzer.h"
#include "sticky/power.h"

namespace {

constexpr int kPinDownButton = 6;
constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;
constexpr uint8_t kResolutionBits = 10;
constexpr uint16_t kDuty = 512;
constexpr uint16_t kPairGapMs = 900;
constexpr uint16_t kAfterChoiceMs = 500;
constexpr uint16_t kDebounceMs = 30;
constexpr uint8_t kFinalTrials = 5;

// Semitone offsets multiply both frequencies by 2^(offset/12).
// Each element retains E14's eight 40 ms notes. Three 50 ms rests
// make every four-element phrase nominally 1430 ms long.
struct Pattern {
  const char* name;
  int8_t offsets[4];
};
constexpr Pattern kPatterns[] = {
    {"user-turn", {0, -1, 2, 1}},
    {"staircase", {0, 1, 2, 3}},
    {"double-call", {0, 2, 0, 2}},
    {"arch", {0, 2, 3, 0}},
    {"question", {0, -2, 0, 3}},
    {"fanfare", {0, 0, 2, 4}},
};
constexpr uint8_t kPatternCount = sizeof(kPatterns) / sizeof(kPatterns[0]);
uint16_t pitches[kPatternCount][4][2]{};
constexpr uint8_t kRoundRobinTrials = kPatternCount * (kPatternCount - 1) / 2;

struct Pair {
  uint8_t left;
  uint8_t right;
};

enum class Phase : uint8_t { RoundRobin, Final, Done };

Pair pairs[kRoundRobinTrials];
uint8_t scores[kPatternCount]{};
uint8_t pairIndex = 0;
uint8_t trialNumber = 0;
uint8_t first = 0;
uint8_t second = 1;
uint8_t finalist[2]{};
uint8_t finalWins[2]{};
uint8_t finalTrial = 0;
uint32_t randomState = 1;
Phase phase = Phase::RoundRobin;
bool started = false;
bool readyToChoose = false;
bool buzzerAttached = false;
bool audioFailed = false;
bool finalReverse = false;

uint32_t randomWord() {
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return randomState;
}

void audioError() {
  audioFailed = true;
  readyToChoose = false;
  ledcWrite(stickyBuzzer::kPin, 0);
  ledcDetach(stickyBuzzer::kPin);
  pinMode(stickyBuzzer::kPin, OUTPUT);
  digitalWrite(stickyBuzzer::kPin, LOW);
  Serial1.println("ERROR: audio output failed; trial invalid, reset required");
}

bool playPattern(uint8_t index) {
  if (!buzzerAttached || audioFailed) return false;
  for (uint8_t element = 0; element < 4; ++element) {
    for (uint8_t note = 0; note < 8; ++note) {
      const uint16_t hz = pitches[index][element][note & 1U];
      if (ledcChangeFrequency(stickyBuzzer::kPin, hz, kResolutionBits) == 0 ||
          !ledcWrite(stickyBuzzer::kPin, kDuty)) {
        audioError();
        return false;
      }
      delay(40);
    }
    if (!ledcWrite(stickyBuzzer::kPin, 0)) {
      audioError();
      return false;
    }
    if (element < 3) delay(50);
  }
  return true;
}

// A held button from playback must not cast a vote on the next pair.
// Require a continuous release interval, including after contact bounce.
void waitForRelease() {
  uint32_t since = millis();
  while (millis() - since < kDebounceMs) {
    if (digitalRead(stickyPower::kPinUpButton) == LOW ||
        digitalRead(stickyPower::kPinAiButton) == LOW ||
        digitalRead(kPinDownButton) == LOW) since = millis();
    delay(5);
  }
}

void playPair() {
  readyToChoose = false;
  delay(250);
  if (!playPattern(first)) return;
  delay(kPairGapMs);
  if (!playPattern(second)) return;
  waitForRelease();
  readyToChoose = true;
  Serial1.println("Choose A/B, or AI to replay.");
}

void shufflePairs() {
  for (uint8_t i = kRoundRobinTrials; i > 1; --i) {
    const uint8_t j = randomWord() % i;
    const Pair temporary = pairs[i - 1];
    pairs[i - 1] = pairs[j];
    pairs[j] = temporary;
  }
}

void printScores() {
  Serial1.println("Round-robin scores:");
  for (uint8_t i = 0; i < kPatternCount; ++i) {
    Serial1.printf("  %-14s %u/%u\n", kPatterns[i].name, scores[i],
                   kPatternCount - 1);
  }
}

void finish() {
  phase = Phase::Done;
  readyToChoose = false;
  const uint8_t winner = finalWins[0] > finalWins[1] ? finalist[0] : finalist[1];
  Serial1.println();
  Serial1.println("E15 complete");
  Serial1.printf("  winner: %s\n", kPatterns[winner].name);
  Serial1.printf("  final: %s %u, %s %u\n",
                 kPatterns[finalist[0]].name, finalWins[0],
                 kPatterns[finalist[1]].name, finalWins[1]);
  Serial1.println("  reset to run again");

  playPattern(winner);
  delay(900);
  playPattern(winner);
}

void prepareTrial();

void beginFinal() {
  printScores();

  // Randomize tie priority, then stable-sort by score. Print all scores
  // so a cutoff tie is visible rather than mistaken for a unique ranking.
  uint8_t rank[kPatternCount];
  for (uint8_t i = 0; i < kPatternCount; ++i) rank[i] = i;
  for (uint8_t i = kPatternCount; i > 1; --i) {
    const uint8_t j = randomWord() % i;
    const uint8_t temporary = rank[i - 1];
    rank[i - 1] = rank[j];
    rank[j] = temporary;
  }
  for (uint8_t i = 1; i < kPatternCount; ++i) {
    const uint8_t key = rank[i];
    uint8_t j = i;
    while (j > 0 && scores[rank[j - 1]] < scores[key]) {
      rank[j] = rank[j - 1];
      --j;
    }
    rank[j] = key;
  }
  finalist[0] = rank[0];
  finalist[1] = rank[1];
  if (scores[rank[1]] == scores[rank[2]])
    Serial1.println("NOTE: tie at finalist cutoff; seeded random priority used");
  finalReverse = (randomWord() & 1U) != 0;

  phase = Phase::Final;
  finalTrial = 0;
  finalWins[0] = 0;
  finalWins[1] = 0;
  Serial1.printf("Finalists: %s and %s\n", kPatterns[finalist[0]].name,
                 kPatterns[finalist[1]].name);
  prepareTrial();
}

void prepareTrial() {
  delay(kAfterChoiceMs);
  if (phase == Phase::RoundRobin) {
    if (pairIndex >= kRoundRobinTrials) {
      beginFinal();
      return;
    }
    first = pairs[pairIndex].left;
    second = pairs[pairIndex].right;
  } else if (phase == Phase::Final) {
    if (finalTrial >= kFinalTrials) {
      finish();
      return;
    }
    first = finalist[0];
    second = finalist[1];
  } else {
    return;
  }

  if (phase == Phase::Final ? ((finalTrial & 1U) != finalReverse)
                            : ((randomWord() & 1U) != 0)) {
    const uint8_t temporary = first;
    first = second;
    second = temporary;
  }
  ++trialNumber;
  if (phase == Phase::RoundRobin) {
    Serial1.printf("\ntrial %u, round robin %u/%u: playing A then B\n",
                   trialNumber, pairIndex + 1, kRoundRobinTrials);
  } else {
    Serial1.printf("\ntrial %u, final %u/%u: playing A then B\n",
                   trialNumber, finalTrial + 1, kFinalTrials);
  }
  playPair();
}

void choose(bool choseFirst) {
  if (!readyToChoose || phase == Phase::Done) return;
  readyToChoose = false;
  const uint8_t chosen = choseFirst ? first : second;
  Serial1.printf("  A = %s, B = %s; chose %s\n",
                 kPatterns[first].name, kPatterns[second].name,
                 choseFirst ? "A" : "B");

  if (phase == Phase::RoundRobin) {
    ++scores[chosen];
    ++pairIndex;
  } else {
    if (chosen == finalist[0]) ++finalWins[0];
    else ++finalWins[1];
    ++finalTrial;
  }
  prepareTrial();
}

bool pressed(int pin) {
  if (digitalRead(pin) != LOW) return false;
  delay(kDebounceMs);
  if (digitalRead(pin) != LOW) return false;
  while (digitalRead(pin) == LOW) delay(5);
  waitForRelease();
  return true;
}

void beginExperiment() {
  uint8_t index = 0;
  for (uint8_t a = 0; a < kPatternCount; ++a) {
    for (uint8_t b = a + 1; b < kPatternCount; ++b) pairs[index++] = {a, b};
  }
  shufflePairs();
  started = true;
  Serial1.println("Round robin: choose the melody you prefer as an energetic notification.");
  prepareTrial();
}

}  // namespace

void setup() {
  stickyPower::holdLatch();
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  for (int pin : {stickyPower::kPinUpButton, stickyPower::kPinAiButton, kPinDownButton}) {
    rtc_gpio_deinit(static_cast<gpio_num_t>(pin));
  }
  pinMode(stickyPower::kPinUpButton, INPUT_PULLUP);
  pinMode(stickyPower::kPinAiButton, INPUT_PULLUP);
  pinMode(kPinDownButton, INPUT_PULLUP);

  for (uint8_t i = 0; i < kPatternCount; ++i) {
    for (uint8_t j = 0; j < 4; ++j) {
      const double factor = pow(2.0, kPatterns[i].offsets[j] / 12.0);
      pitches[i][j][0] = static_cast<uint16_t>(lround(3550 * factor));
      pitches[i][j][1] = static_cast<uint16_t>(lround(4150 * factor));
    }
  }
  randomState = esp_random();
  if (randomState == 0) randomState = 1;
  const uint32_t seed = randomState;
  gpio_hold_dis(static_cast<gpio_num_t>(stickyBuzzer::kPin));
  buzzerAttached = ledcAttach(stickyBuzzer::kPin, 3900, kResolutionBits);
  if (buzzerAttached && !ledcWrite(stickyBuzzer::kPin, 0)) audioError();

  Serial1.println();
  Serial1.printf("E15 notification melody rig -- seed %lu\n",
                 static_cast<unsigned long>(seed));
  Serial1.println("Up chooses A; AI starts/replays; Down chooses B.");
  Serial1.println("Choose the melody you would prefer as an energetic notification.");
  Serial1.println("Do not watch this log while choosing.");
  if (!buzzerAttached) Serial1.println("ERROR: could not attach LEDC to the buzzer");
  else if (!audioFailed) Serial1.println("Press AI when ready.");
  waitForRelease();
}

void loop() {
  if (!buzzerAttached || audioFailed) {
    delay(100);
    return;
  }
  if (pressed(stickyPower::kPinUpButton)) {
    if (started) choose(true);
  } else if (pressed(stickyPower::kPinAiButton)) {
    if (!started) beginExperiment();
    else if (readyToChoose) {
      Serial1.printf("replay trial %u\n", trialNumber);
      playPair();
    }
  } else if (pressed(kPinDownButton)) {
    if (started) choose(false);
  }
  delay(5);
}
