#pragma once
#include "reminder_state.h"
#include <ArduinoJson.h>

namespace reminders {
void begin(bool cold);
reminder::State& state();
int64_t nowMs();
bool due();
uint64_t sleepUs(uint64_t other);
bool accept(JsonVariantConst json, uint64_t receivedTicks, int64_t ageMs);
// Independent bounded worker; polled by the orchestrator alongside dashboard.
void startSync();
void pollSync();
void cancelSync();
bool syncing();
}
