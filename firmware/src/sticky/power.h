#pragma once

#include <esp_sleep.h>
#include <esp_system.h>
#include <stdint.h>

// reTerminal Sticky (E1005) power rail latch and deep sleep entry.
//
// The board holds its own rail up through a latch: GPIO45 (PWR_HOLD) and
// GPIO46 (PWR_LOCK) have to be driven high early in boot or the board cuts
// itself off once the power button is released. The same applies across deep
// sleep -- the pads have to be latched with gpio_hold_en() before sleeping, or
// the board powers down instead of sleeping and the next press looks like a
// cold boot rather than a wake.
//
// On USB the rail is fed externally, so neither failure shows up on the desk;
// both only appear on battery. Pin names and sequence follow Seeed's own
// firmware (reTerminal_Sticky_Bunny: src/board/pin_config.h,
// src/board/board_power.cpp).
//
// Both latch pins are ESP32-S3 strapping pins, sampled only at reset, so taking
// them over as outputs afterwards is safe.

namespace stickyPower {

constexpr int kPinHold = 45;      // PWR_HOLD
constexpr int kPinLock = 46;      // PWR_LOCK
constexpr int kPinAiButton = 4;   // AI button, active low, the wake source

// Drives the latch high. First thing in setup(), before anything else draws on
// the rail; also clears any pad hold a previous deep sleep left armed, since
// writes land on a latched pad without effect.
void holdLatch();

// Everything about going to sleep except the final esp_deep_sleep_start():
// parks the peripheral enables, latches the power pins through the sleep and
// arms the wake sources -- the AI button (ext1, any-low) always, and the timer
// as well when timerWakeUs is non-zero.
//
// Split from the entry itself so a caller can take a timestamp as late as
// possible before the chip goes down; see src/experiments/e1_wake_latency.cpp.
void prepareDeepSleep(uint64_t timerWakeUs = 0);

// esp_deep_sleep_start(). Does not return.
[[noreturn]] void enterDeepSleep();

// prepareDeepSleep() followed by enterDeepSleep().
[[noreturn]] void deepSleep(uint64_t timerWakeUs = 0);

// True when this boot came out of deep sleep rather than a reset or a power-on.
// Worth checking before trusting any wake measurement: a board that dropped its
// latch and switched off comes back as a power-on and takes a different path
// through the bootloader.
bool wokeFromDeepSleep();

// Short names for esp_reset_reason() and esp_sleep_get_wakeup_cause(). The
// value-taking forms exist so a reason recorded earlier -- in RTC memory across
// a sleep, say -- can be printed long after the boot it belongs to.
const char* resetReasonName(esp_reset_reason_t reason);
const char* wakeupCauseName(esp_sleep_wakeup_cause_t cause);
const char* resetReasonName();
const char* wakeupCauseName();

}  // namespace stickyPower
