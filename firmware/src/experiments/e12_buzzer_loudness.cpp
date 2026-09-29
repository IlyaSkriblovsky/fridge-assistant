// E12: subjective search for the loudest useful drive of the onboard buzzer.
//
// Controls (all buttons are active low):
//   Up / GPIO5   choose the first sound (A)
//   AI / GPIO4   start the experiment, or replay the current pair
//   Down / GPIO6 choose the second sound (B)
//
// Do not watch the UART while choosing: it reveals the two signals after each
// vote. The playback order is randomised independently for every comparison.

#include <Arduino.h>
#include <esp_system.h>

#include "sticky/buzzer.h"
#include "sticky/power.h"

namespace {

constexpr int kPinDownButton = 6;
constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

constexpr uint8_t kResolutionBits = 10;
constexpr uint16_t kFullDuty = 1U << kResolutionBits;
constexpr uint16_t kToneMs = 350;
constexpr uint16_t kPairGapMs = 650;
constexpr uint16_t kAfterChoiceMs = 500;
constexpr uint16_t kDebounceMs = 30;

struct Signal {
  uint16_t hz;
  uint16_t duty;
};

constexpr uint16_t kFrequencies[] = {
    1000, 1250, 1500, 1750, 2000, 2250, 2500,
    2750, 3000, 3250, 3500, 3750, 4000, 4250,
    4500, 4750, 5000, 5250, 5500, 5750, 6000,
};
constexpr uint16_t kDuties[] = {128, 256, 384, 512};  // 12.5, 25, 37.5, 50%
constexpr size_t kMaxCandidates = sizeof(kFrequencies) / sizeof(kFrequencies[0]);
constexpr uint8_t kConfirmationTrials = 3;

enum class Phase : uint8_t { Frequency, Duty, Confirmation, Done };

Signal pool[kMaxCandidates];
Signal nextPool[kMaxCandidates];
size_t poolCount = 0;
size_t pairIndex = 0;
size_t nextCount = 0;
uint8_t roundNumber = 0;
uint16_t trialNumber = 0;

Phase phase = Phase::Frequency;
Signal first{};
Signal second{};
Signal frequencyWinner{};
Signal optimized{};
bool readyToChoose = false;
bool started = false;
bool buzzerAttached = false;
uint32_t randomState = 1;
uint8_t confirmationDone = 0;
uint8_t optimizedWins = 0;
bool referenceIdentical = false;

uint32_t randomWord() {
  // Keep the printed seed sufficient to reproduce the presentation order.
  randomState ^= randomState << 13;
  randomState ^= randomState >> 17;
  randomState ^= randomState << 5;
  return randomState;
}

bool sameSignal(const Signal& a, const Signal& b) {
  return a.hz == b.hz && a.duty == b.duty;
}

void printSignal(const Signal& signal) {
  Serial1.printf("%u Hz, %.1f%%", signal.hz,
                 100.0 * signal.duty / kFullDuty);
}

void play(const Signal& signal) {
  if (!buzzerAttached) return;
  if (ledcChangeFrequency(stickyBuzzer::kPin, signal.hz, kResolutionBits) == 0) {
    Serial1.printf("ERROR: LEDC rejected %u Hz\n", signal.hz);
    return;
  }
  ledcWrite(stickyBuzzer::kPin, signal.duty);
  delay(kToneMs);
  ledcWrite(stickyBuzzer::kPin, 0);
}

void playPair() {
  readyToChoose = false;
  delay(250);
  play(first);
  delay(kPairGapMs);
  play(second);
  readyToChoose = true;
}

void shufflePool() {
  for (size_t i = poolCount; i > 1; --i) {
    const size_t j = randomWord() % i;
    const Signal temporary = pool[i - 1];
    pool[i - 1] = pool[j];
    pool[j] = temporary;
  }
}

const char* phaseName() {
  switch (phase) {
    case Phase::Frequency: return "frequency";
    case Phase::Duty: return "duty";
    case Phase::Confirmation: return "confirmation";
    case Phase::Done: return "done";
  }
  return "unknown";
}

void prepareTrial();

void finish() {
  phase = Phase::Done;
  readyToChoose = false;
  Serial1.println();
  Serial1.println("E12 complete");
  Serial1.print("  frequency winner: ");
  printSignal(frequencyWinner);
  Serial1.println();
  Serial1.print("  optimized signal: ");
  printSignal(optimized);
  Serial1.println();
  if (referenceIdentical) {
    Serial1.println("  optimized signal is identical to the production reference");
  } else {
    Serial1.printf("  optimized beat the production reference in %u/%u trials\n",
                   optimizedWins, kConfirmationTrials);
  }
  Serial1.println("  reset to run again; repeat close runs before changing production");

  const Signal done[] = {{1500, 512}, {2500, 512}, {3500, 512}};
  for (const Signal& signal : done) {
    play(signal);
    delay(80);
  }
}

void beginConfirmation() {
  phase = Phase::Confirmation;
  confirmationDone = 0;
  optimizedWins = 0;
  const Signal production = {2500, 512};
  referenceIdentical = sameSignal(optimized, production);
  Serial1.println();
  if (referenceIdentical) {
    Serial1.println("Stage 3 skipped: optimized signal equals production 2500 Hz / 50%");
    finish();
    return;
  }
  Serial1.println("Stage 3: three blind comparisons with production 2500 Hz / 50%");
  prepareTrial();
}

void finishTournament(const Signal& winner) {
  if (phase == Phase::Frequency) {
    frequencyWinner = winner;
    Serial1.println();
    Serial1.print("Frequency winner: ");
    printSignal(frequencyWinner);
    Serial1.println();
    Serial1.println("Stage 2: duty tournament at the winning frequency");

    phase = Phase::Duty;
    poolCount = sizeof(kDuties) / sizeof(kDuties[0]);
    for (size_t i = 0; i < poolCount; ++i) pool[i] = {winner.hz, kDuties[i]};
    shufflePool();
    pairIndex = 0;
    nextCount = 0;
    roundNumber = 1;
    prepareTrial();
    return;
  }

  optimized = winner;
  Serial1.println();
  Serial1.print("Duty winner: ");
  printSignal(optimized);
  Serial1.println();
  beginConfirmation();
}

void prepareTournamentTrial() {
  for (;;) {
    if (pairIndex + 1 < poolCount) break;

    if (pairIndex < poolCount) nextPool[nextCount++] = pool[pairIndex];
    if (nextCount == 1) {
      finishTournament(nextPool[0]);
      return;
    }

    poolCount = nextCount;
    for (size_t i = 0; i < poolCount; ++i) pool[i] = nextPool[i];
    shufflePool();
    pairIndex = 0;
    nextCount = 0;
    ++roundNumber;
  }

  first = pool[pairIndex];
  second = pool[pairIndex + 1];
  if (randomWord() & 1U) {
    const Signal temporary = first;
    first = second;
    second = temporary;
  }

  ++trialNumber;
  Serial1.printf("\ntrial %u, %s round %u: playing A then B\n",
                 trialNumber, phaseName(), roundNumber);
  playPair();
}

void prepareConfirmationTrial() {
  if (confirmationDone >= kConfirmationTrials) {
    finish();
    return;
  }

  const Signal production = {2500, 512};
  first = optimized;
  second = production;
  if (randomWord() & 1U) {
    first = production;
    second = optimized;
  }
  ++trialNumber;
  Serial1.printf("\ntrial %u, confirmation %u/%u: playing A then B\n",
                 trialNumber, confirmationDone + 1, kConfirmationTrials);
  playPair();
}

void prepareTrial() {
  delay(kAfterChoiceMs);
  if (phase == Phase::Confirmation) prepareConfirmationTrial();
  else if (phase != Phase::Done) prepareTournamentTrial();
}

void choose(bool choseFirst) {
  if (!readyToChoose || phase == Phase::Done) return;
  readyToChoose = false;
  const Signal chosen = choseFirst ? first : second;

  Serial1.print("  A = ");
  printSignal(first);
  Serial1.print(", B = ");
  printSignal(second);
  Serial1.print("; chose ");
  Serial1.println(choseFirst ? "A" : "B");

  if (phase == Phase::Confirmation) {
    if (sameSignal(chosen, optimized)) ++optimizedWins;
    ++confirmationDone;
  } else {
    nextPool[nextCount++] = chosen;
    pairIndex += 2;
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
  started = true;
  phase = Phase::Frequency;
  poolCount = kMaxCandidates;
  for (size_t i = 0; i < poolCount; ++i) pool[i] = {kFrequencies[i], 512};
  shufflePool();
  pairIndex = 0;
  nextCount = 0;
  roundNumber = 1;
  trialNumber = 0;
  Serial1.println("Stage 1: frequency tournament at 50% duty");
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

  buzzerAttached = ledcAttach(stickyBuzzer::kPin, 2500, kResolutionBits);
  if (buzzerAttached) ledcWrite(stickyBuzzer::kPin, 0);

  Serial1.println();
  Serial1.printf("E12 buzzer loudness rig -- seed %lu\n",
                 static_cast<unsigned long>(seed));
  Serial1.println("Up chooses A; AI starts/replays; Down chooses B.");
  Serial1.println("Listen from a fixed position and do not watch this log while choosing.");
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
