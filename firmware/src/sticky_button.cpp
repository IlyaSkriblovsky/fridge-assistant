#include "sticky_button.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>

#include "config.h"

void StickyButton::begin(int64_t pressStartUs) {
  _pressStartUs = pressStartUs;
  _highSinceUs = 0;
  _heldUs = 0;
  _released = false;

  // ext1 leaves GPIO4 as an RTC pad, and while it is one the digital
  // peripheral reads nothing: gpio_get_level() would report a level the pin is
  // not at. rtc_gpio_deinit() hands the pad back to the GPIO matrix, and only
  // then does the pull-up below mean anything.
  rtc_gpio_deinit(static_cast<gpio_num_t>(kPin));
  pinMode(kPin, INPUT_PULLUP);
}

bool StickyButton::isDown() const {
  return gpio_get_level(static_cast<gpio_num_t>(kPin)) == 0;
}

bool StickyButton::poll() {
  if (_released) return true;

  if (isDown()) {
    // Any low restarts the window, so a bounce mid-release does not end the
    // press. This is also what makes a press that is still held cost a single
    // register read per call.
    _highSinceUs = 0;
    return false;
  }

  const int64_t now = esp_timer_get_time();
  if (_highSinceUs == 0) {
    _highSinceUs = now;
    return false;
  }
  if (now - _highSinceUs < static_cast<int64_t>(config::kButtonDebounceMs) * 1000) {
    return false;
  }

  _heldUs = _highSinceUs - _pressStartUs;
  _released = true;
  return true;
}

uint32_t StickyButton::heldMs() const {
  const int64_t us = _released ? _heldUs : esp_timer_get_time() - _pressStartUs;
  return static_cast<uint32_t>(us / 1000);
}

bool StickyButton::isTap() const { return heldMs() < config::kButtonMinHoldMs; }
