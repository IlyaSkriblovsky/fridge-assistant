// E14: blind preference test for the pitch span of the E13 two-note warble.
//
// Controls (all buttons are active low):
//   Up / GPIO5   choose the first trill (A)
//   AI / GPIO4   start the experiment, or replay the current pair
//   Down / GPIO6 choose the second trill (B)

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
constexpr uint16_t kNoteMs = 40;
constexpr uint16_t kPairGapMs = 700;
constexpr uint16_t kAfterChoiceMs = 500;
constexpr uint16_t kDebounceMs = 30;
constexpr uint8_t kCandidateCount = 4;
constexpr uint8_t kRepeatsPerPair = 2;
constexpr uint8_t kRoundRobinTrials =
    kCandidateCount * (kCandidateCount - 1) / 2 * kRepeatsPerPair;
constexpr uint8_t kFinalTrials = 5;

struct Candidate {
  const char* name;
  uint16_t lowHz;
  uint16_t highHz;
};

// All candidates share a 3850 Hz centre. Only the interval changes.
constexpr Candidate kCandidates[] = {
    {"span-400", 3650, 4050},
    {"span-600", 3550, 4150},
    {"span-800", 3450, 4250},
    {"span-1000", 3350, 4350},
};

struct Pair {
  uint8_t left;
  uint8_t right;
};

enum class Phase : uint8_t { RoundRobin, Final, Done };

Pair pairs[kRoundRobinTrials];
uint8_t scores[kCandidateCount]{};
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

void playCandidate(uint8_t index) {
  if (!buzzerAttached) return;
  const Candidate& candidate = kCandidates[index];
  for (uint8_t note = 0; note < 8; ++note) {
    const uint16_t hz = (note & 1U) ? candidate.highHz : candidate.lowHz;
    if (ledcChangeFrequency(stickyBuzzer::kPin, hz, kResolutionBits) != 0) {
      ledcWrite(stickyBuzzer::kPin, kDuty);
    } else {
      Serial1.printf("ERROR: LEDC rejected %u Hz\n", hz);
      ledcWrite(stickyBuzzer::kPin, 0);
    }
    delay(kNoteMs);
  }
  ledcWrite(stickyBuzzer::kPin, 0);
}

void playPair() {
  readyToChoose = false;
  delay(250);
  playCandidate(first);
  delay(kPairGapMs);
  playCandidate(second);
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
  Serial1.println("Repeated round-robin scores:");
  for (uint8_t i = 0; i < kCandidateCount; ++i) {
    Serial1.printf("  %-10s %u/%u\n", kCandidates[i].name, scores[i],
                   (kCandidateCount - 1) * kRepeatsPerPair);
  }
}

void finish() {
  phase = Phase::Done;
  readyToChoose = false;
  const uint8_t winner = finalWins[0] > finalWins[1] ? finalist[0] : finalist[1];
  Serial1.println();
  Serial1.println("E14 complete");
  Serial1.printf("  winner: %s (%u--%u Hz)\n", kCandidates[winner].name,
                 kCandidates[winner].lowHz, kCandidates[winner].highHz);
  Serial1.printf("  final: %s %u, %s %u\n",
                 kCandidates[finalist[0]].name, finalWins[0],
                 kCandidates[finalist[1]].name, finalWins[1]);
  Serial1.println("  reset to run again");
  playCandidate(winner);
  delay(120);
  playCandidate(winner);
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
  for (uint8_t i = 2; i < kCandidateCount; ++i) {
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
  Serial1.printf("Finalists: %s and %s\n", kCandidates[finalist[0]].name,
                 kCandidates[finalist[1]].name);
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
    Serial1.printf("\ntrial %u, repeated round robin %u/%u: playing A then B\n",
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
                 kCandidates[first].name, kCandidates[second].name,
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
  for (uint8_t repeat = 0; repeat < kRepeatsPerPair; ++repeat) {
    for (uint8_t a = 0; a < kCandidateCount; ++a) {
      for (uint8_t b = a + 1; b < kCandidateCount; ++b) pairs[index++] = {a, b};
    }
  }
  shufflePairs();
  started = true;
  Serial1.println("Choose the trill span you prefer as an energetic notification.");
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
  buzzerAttached = ledcAttach(stickyBuzzer::kPin, 3850, kResolutionBits);
  if (buzzerAttached) ledcWrite(stickyBuzzer::kPin, 0);

  Serial1.println();
  Serial1.printf("E14 trill span rig -- seed %lu\n", static_cast<unsigned long>(seed));
  Serial1.println("Up chooses A; AI starts/replays; Down chooses B.");
  Serial1.println("Choose the trill you prefer as an energetic notification.");
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
