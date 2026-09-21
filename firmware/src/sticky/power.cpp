#include "sticky/power.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <driver/rtc_io.h>
#include <esp_sleep.h>
#include <esp_system.h>
#include "config.h"

namespace {
bool upWake = false;

// Peripheral enables parked low for the duration of the sleep, following
// Seeed's own firmware. SD_EN is deliberately absent: sources disagree on
// whether the Sticky has one at all (see docs/project-vision.md).
constexpr gpio_num_t kParkLow[] = {
    GPIO_NUM_47,  // EPD_EN, e-paper panel power
    GPIO_NUM_42,  // TOUCH_EN
    GPIO_NUM_41,  // TOUCH_RST
    GPIO_NUM_48,  // BUZZER
    GPIO_NUM_38,  // MIC power enable (TPS22916)
};

}  // namespace

void stickyPower::holdLatch() {
  // Clear any pad holds a previous deep sleep left armed before driving the
  // pins, otherwise the writes land on latched pads.
  gpio_deep_sleep_hold_dis();
  gpio_hold_dis(static_cast<gpio_num_t>(kPinHold));
  gpio_hold_dis(static_cast<gpio_num_t>(kPinLock));

  pinMode(kPinHold, OUTPUT);
  digitalWrite(kPinHold, HIGH);
  pinMode(kPinLock, OUTPUT);
  digitalWrite(kPinLock, HIGH);

  // A cold start is the only path where the rail is actually coming up. On a
  // deep sleep wake the pads were held through the sleep, so the latch never
  // went low and the rail never dropped -- there is nothing to settle.
  if (!wokeFromDeepSleep()) {
    delay(100);  // let the rail settle before anything else draws on it
  }
}

void stickyPower::enableUpWake() {
  upWake = true;
  rtc_gpio_deinit(static_cast<gpio_num_t>(kPinUpButton));
  pinMode(kPinUpButton, INPUT_PULLUP);
}

void stickyPower::waitForWakeButtonsReleased() {
  uint32_t since = millis();
  while (millis() - since < config::kButtonDebounceMs) {
    if (digitalRead(kPinAiButton) == LOW ||
        (upWake && digitalRead(kPinUpButton) == LOW)) since = millis();
    delay(5);
  }
}

void stickyPower::prepareDeepSleep(uint64_t timerWakeUs) {
  for (gpio_num_t pin : kParkLow) {
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, 0);
  }

  // The AI button is active low, so its pull-up has to survive the sleep. The
  // internal pull-up on an RTC pad only holds while the RTC peripheral domain
  // stays powered, which costs idle current -- the alternative is trusting an
  // external pull-up on this board that has not been confirmed.
  const gpio_num_t button = static_cast<gpio_num_t>(kPinAiButton);
  rtc_gpio_pullup_en(button);
  rtc_gpio_pulldown_dis(button);
  esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON);
  uint64_t mask = BIT64(kPinAiButton);
  if (upWake) {
    rtc_gpio_pullup_en(static_cast<gpio_num_t>(kPinUpButton));
    rtc_gpio_pulldown_dis(static_cast<gpio_num_t>(kPinUpButton));
    mask |= BIT64(kPinUpButton);
  }
  esp_sleep_enable_ext1_wakeup_io(mask, ESP_EXT1_WAKEUP_ANY_LOW);

  if (timerWakeUs > 0) {
    esp_sleep_enable_timer_wakeup(timerWakeUs);
  }

  // GPIO45/46 are digital pads, not RTC pads, so the digital hold is what keeps
  // the latch up once the digital domain loses power. Without this the board
  // switches off instead of sleeping.
  gpio_hold_en(static_cast<gpio_num_t>(kPinHold));
  gpio_hold_en(static_cast<gpio_num_t>(kPinLock));
  gpio_deep_sleep_hold_en();
}

void stickyPower::enterDeepSleep() {
  esp_deep_sleep_start();
  // esp_deep_sleep_start() does not return; the chip resets on wake.
  for (;;) {
  }
}

void stickyPower::deepSleep(uint64_t timerWakeUs) {
  prepareDeepSleep(timerWakeUs);
  enterDeepSleep();
}

bool stickyPower::wokeFromDeepSleep() {
  return esp_reset_reason() == ESP_RST_DEEPSLEEP;
}

const char* stickyPower::resetReasonName() { return resetReasonName(esp_reset_reason()); }

const char* stickyPower::wakeupCauseName() {
  return wakeupCauseName(esp_sleep_get_wakeup_cause());
}

const char* stickyPower::resetReasonName(esp_reset_reason_t reason) {
  switch (reason) {
    case ESP_RST_POWERON: return "POWERON";
    case ESP_RST_EXT: return "EXT";
    case ESP_RST_SW: return "SW";
    case ESP_RST_PANIC: return "PANIC";
    case ESP_RST_INT_WDT: return "INT_WDT";
    case ESP_RST_TASK_WDT: return "TASK_WDT";
    case ESP_RST_WDT: return "WDT";
    case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
    case ESP_RST_BROWNOUT: return "BROWNOUT";
    case ESP_RST_SDIO: return "SDIO";
    default: return "UNKNOWN";
  }
}

const char* stickyPower::wakeupCauseName(esp_sleep_wakeup_cause_t cause) {
  switch (cause) {
    case ESP_SLEEP_WAKEUP_UNDEFINED: return "none";
    case ESP_SLEEP_WAKEUP_EXT0: return "ext0";
    case ESP_SLEEP_WAKEUP_EXT1: return "ext1";
    case ESP_SLEEP_WAKEUP_TIMER: return "timer";
    case ESP_SLEEP_WAKEUP_TOUCHPAD: return "touch";
    case ESP_SLEEP_WAKEUP_ULP: return "ulp";
    case ESP_SLEEP_WAKEUP_GPIO: return "gpio";
    case ESP_SLEEP_WAKEUP_UART: return "uart";
    default: return "other";
  }
}
