// E13: blind preference test for an energetic notification trill.
//
// Controls (all buttons are active low):
//   Up / GPIO5   choose the first trill (A)
//   AI / GPIO4   start the experiment, or replay the current pair
//   Down / GPIO6 choose the second trill (B)
//
// Do not watch the UART while choosing: it reveals pattern names after a vote.

#include <Arduino.h>
#include <esp_system.h>

#include "sticky/buzzer.h"
#include "sticky/power.h"

namespace {

constexpr int kPinDownButton = 6;
constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;
constexpr uint8_t kResolutionBits = 10;
constexpr uint16_t kDuty = 512;
constexpr uint16_t kPairGapMs = 700;
constexpr uint16_t kAfterChoiceMs = 500;
constexpr uint16_t kDebounceMs = 30;
constexpr uint8_t kFinalTrials = 3;

struct Step {
  uint16_t hz;  // zero is a rest
  uint16_t ms;
};

struct Pattern {
  const char* name;
  const Step* steps;
  size_t count;
};

// Each candidate lasts 320 ms. Frequencies cluster around the broad 3.75--4.0
// kHz winner from E12; the experiment is about contour and rhythm.
constexpr Step kCloseWarble[] = {
    {3650, 40}, {4050, 40}, {3650, 40}, {4050, 40},
    {3650, 40}, {4050, 40}, {3650, 40}, {4050, 40},
};
constexpr Step kWideWarble[] = {
    {3300, 40}, {4300, 40}, {3300, 40}, {4300, 40},
    {3300, 40}, {4300, 40}, {3300, 40}, {4300, 40},
};
constexpr Step kDoubleRise[] = {
    {3400, 45}, {3750, 45}, {4150, 60}, {0, 20},
    {3400, 45}, {3750, 45}, {4150, 60},
};
constexpr Step kRhythmic[] = {
    {3900, 55}, {0, 15}, {4200, 55}, {0, 25},
    {3700, 45}, {4050, 45}, {4350, 80},
};
constexpr Step kSpark[] = {
    {3500, 35}, {4300, 45}, {3800, 35}, {4400, 45},
    {3700, 35}, {4200, 45}, {3900, 35}, {4500, 45},
};
constexpr Step kArc[] = {
    {3350, 40}, {3600, 40}, {3900, 40}, {4200, 40},
    {4400, 40}, {4100, 40}, {3800, 40}, {3500, 40},
};

constexpr Pattern kPatterns[] = {
    {"close-warble", kCloseWarble, sizeof(kCloseWarble) / sizeof(kCloseWarble[0])},
    {"wide-warble", kWideWarble, sizeof(kWideWarble) / sizeof(kWideWarble[0])},
    {"double-rise", kDoubleRise, sizeof(kDoubleRise) / sizeof(kDoubleRise[0])},
    {"rhythmic", kRhythmic, sizeof(kRhythmic) / sizeof(kRhythmic[0])},
    {"spark", kSpark, sizeof(kSpark) / sizeof(kSpark[0])},
    {"arc", kArc, sizeof(kArc) / sizeof(kArc[0])},
};
constexpr uint8_t kPatternCount = sizeof(kPatterns) / sizeof(kPatterns[0]);
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

uint32_t randomWord() {
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return randomState;
}

void playPattern(uint8_t index) {
  if (!buzzerAttached) return;
  const Pattern& pattern = kPatterns[index];
  for (size_t i = 0; i < pattern.count; ++i) {
    const Step& step = pattern.steps[i];
    if (step.hz == 0) {
      ledcWrite(stickyBuzzer::kPin, 0);
    } else if (ledcChangeFrequency(stickyBuzzer::kPin, step.hz,
                                   kResolutionBits) != 0) {
      ledcWrite(stickyBuzzer::kPin, kDuty);
    } else {
      Serial1.printf("ERROR: LEDC rejected %u Hz\n", step.hz);
      ledcWrite(stickyBuzzer::kPin, 0);
    }
    delay(step.ms);
  }
  ledcWrite(stickyBuzzer::kPin, 0);
}

void playPair() {
  readyToChoose = false;
  delay(250);
  playPattern(first);
  delay(kPairGapMs);
  playPattern(second);
  readyToChoose = true;
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
  Serial1.println("E13 complete");
  Serial1.printf("  winner: %s\n", kPatterns[winner].name);
  Serial1.printf("  final: %s %u, %s %u\n",
                 kPatterns[finalist[0]].name, finalWins[0],
                 kPatterns[finalist[1]].name, finalWins[1]);
  Serial1.println("  reset to run again");

  playPattern(winner);
  delay(120);
  playPattern(winner);
}

void prepareTrial();

void beginFinal() {
  printScores();

  finalist[0] = 0;
  finalist[1] = 1;
  if (scores[finalist[1]] > scores[finalist[0]]) {
    const uint8_t temporary = finalist[0];
    finalist[0] = finalist[1];
    finalist[1] = temporary;
  }
  for (uint8_t i = 2; i < kPatternCount; ++i) {
    if (scores[i] > scores[finalist[0]]) {
      finalist[1] = finalist[0];
      finalist[0] = i;
    } else if (scores[i] > scores[finalist[1]]) {
      finalist[1] = i;
    }
  }

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

  if (randomWord() & 1U) {
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
  delay(kDebounceMs);
  return true;
}

void beginExperiment() {
  uint8_t index = 0;
  for (uint8_t a = 0; a < kPatternCount; ++a) {
    for (uint8_t b = a + 1; b < kPatternCount; ++b) pairs[index++] = {a, b};
  }
  shufflePairs();
  started = true;
  Serial1.println("Round robin: choose the trill you prefer as an energetic notification.");
  prepareTrial();
}

}  // namespace

void setup() {
  stickyPower::holdLatch();
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  pinMode(stickyPower::kPinUpButton, INPUT_PULLUP);
  pinMode(stickyPower::kPinAiButton, INPUT_PULLUP);
  pinMode(kPinDownButton, INPUT_PULLUP);

  randomState = esp_random();
  if (randomState == 0) randomState = 1;
  const uint32_t seed = randomState;
  buzzerAttached = ledcAttach(stickyBuzzer::kPin, 3900, kResolutionBits);
  if (buzzerAttached) ledcWrite(stickyBuzzer::kPin, 0);

  Serial1.println();
  Serial1.printf("E13 notification trill rig -- seed %lu\n",
                 static_cast<unsigned long>(seed));
  Serial1.println("Up chooses A; AI starts/replays; Down chooses B.");
  Serial1.println("Choose the trill you would prefer as an energetic notification.");
  Serial1.println("Do not watch this log while choosing.");
  if (!buzzerAttached) Serial1.println("ERROR: could not attach LEDC to the buzzer");
  else Serial1.println("Press AI when ready.");
}

void loop() {
  if (!buzzerAttached) {
    delay(100);
    return;
  }
  if (pressed(stickyPower::kPinUpButton)) {
    if (started) choose(true);
  } else if (pressed(stickyPower::kPinAiButton)) {
    if (!started) beginExperiment();
    else if (readyToChoose) playPair();
  } else if (pressed(kPinDownButton)) {
    if (started) choose(false);
  }
  delay(5);
}
