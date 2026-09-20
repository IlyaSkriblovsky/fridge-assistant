#include "display.h"

#include <esp_timer.h>

#include "text.h"

namespace {

// Set while nothing is waiting and the panel is not refreshing. Cleared by a
// post, under the same lock that fills the slot, and set by the task under that
// lock once it finds the slot empty -- so the bit and the slot never disagree.
constexpr EventBits_t kIdleBit = BIT0;

}  // namespace

Display::~Display() {
  if (_task != nullptr) {
    // Idle means blocked on its notification and holding nothing, which is the
    // one state it is safe to delete it in.
    waitIdle(UINT32_MAX);
    vTaskDelete(_task);
  }
  if (_events != nullptr) vEventGroupDelete(_events);
  if (_lock != nullptr) vSemaphoreDelete(_lock);
}

bool Display::start() {
  if (_task != nullptr) return true;

  if (!_screen.begin()) {
    _lastError = _screen.lastError();
    return false;
  }

  if (_lock == nullptr) _lock = xSemaphoreCreateMutex();
  if (_events == nullptr) _events = xEventGroupCreate();
  if (_lock == nullptr || _events == nullptr) {
    _lastError = "no memory for the display task's lock";
    return false;
  }
  xEventGroupSetBits(_events, kIdleBit);

  if (xTaskCreatePinnedToCore(trampoline, "display", kStackBytes, this, kPriority, &_task,
                              kCore) != pdPASS) {
    _task = nullptr;
    _lastError = "display task could not be created";
    return false;
  }
  return true;
}

void Display::clear() {
  if (_task == nullptr) return;

  xSemaphoreTake(_lock, portMAX_DELAY);
  _clearPending = true;
  _records[static_cast<uint8_t>(Screen::Clear)].postedUs = esp_timer_get_time();
  xEventGroupClearBits(_events, kIdleBit);
  xSemaphoreGive(_lock);

  xTaskNotifyGive(_task);
}

void Display::listening() { post(Screen::Listening, nullptr, nullptr); }

void Display::working() { post(Screen::Working, nullptr, nullptr); }

void Display::answer(const char* text) { post(Screen::Answer, text, nullptr); }

void Display::error(const char* title, const char* detail) { post(Screen::Error, title, detail); }

void Display::post(Screen screen, const char* text, const char* detail) {
  if (_task == nullptr) return;

  // Copied through textCopy() rather than raw, so that the slot's bound --
  // which is in bytes, because the buffer is -- falls between characters. A
  // Cyrillic answer cut at a byte count would end in half a letter.
  Slot next;
  next.screen = screen;
  textCopy(next.text, sizeof(next.text), text);
  textCopy(next.detail, sizeof(next.detail), detail);

  xSemaphoreTake(_lock, portMAX_DELAY);
  if (_slot.screen != Screen::Count) {
    _records[static_cast<uint8_t>(_slot.screen)].superseded = true;
  }
  _slot = next;
  Record& record = _records[static_cast<uint8_t>(screen)];
  record.postedUs = esp_timer_get_time();
  record.superseded = false;
  xEventGroupClearBits(_events, kIdleBit);
  xSemaphoreGive(_lock);

  xTaskNotifyGive(_task);
}

bool Display::waitIdle(uint32_t timeoutMs) {
  if (_task == nullptr) return true;

  const TickType_t ticks = timeoutMs == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs);
  // The bits are set and waited on inside the kernel's own critical sections,
  // which carry the barrier: everything the task wrote into the records before
  // it set the bit is visible to whoever comes back out of here.
  return (xEventGroupWaitBits(_events, kIdleBit, pdFALSE, pdTRUE, ticks) & kIdleBit) != 0;
}

uint32_t Display::stackUnusedBytes() const {
  if (_task == nullptr) return 0;
  // StackType_t is a byte on this port, so the high-water mark is in bytes.
  return static_cast<uint32_t>(uxTaskGetStackHighWaterMark(_task));
}

void Display::trampoline(void* self) { static_cast<Display*>(self)->run(); }

void Display::run() {
  for (;;) {
    Slot slot;

    xSemaphoreTake(_lock, portMAX_DELAY);
    if (_clearPending) {
      // Ahead of the slot, whatever is in it: the pre-clear is what makes the
      // first real screen of a cold start safe to draw.
      _clearPending = false;
      slot.screen = Screen::Clear;
    } else if (_slot.screen != Screen::Count) {
      slot = _slot;
      _slot.screen = Screen::Count;
    } else {
      xEventGroupSetBits(_events, kIdleBit);
      xSemaphoreGive(_lock);
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
      continue;
    }
    _records[static_cast<uint8_t>(slot.screen)].startUs = esp_timer_get_time();
    xSemaphoreGive(_lock);

    draw(slot);
  }
}

void Display::draw(const Slot& slot) {
  // Only the fields post() never writes, so nothing here needs the lock.
  Record& record = _records[static_cast<uint8_t>(slot.screen)];
  bool refused = false;

  switch (slot.screen) {
    case Screen::Clear:
      _screen.clear();
      break;
    case Screen::Listening:
      _screen.listening();
      break;
    case Screen::Working:
      // Refused leaves LISTENING on the glass until the answer lands, which is
      // not worth failing a question over -- see StickyScreen::working().
      refused = !_screen.working();
      break;
    case Screen::Answer:
      // Full with a reason is a partial the controller refused. Full without
      // one is the first screen of a boot, which has no full refresh behind it
      // and takes one by design.
      _screen.answer(slot.text);
      refused = !_screen.lastWasPartial() && _screen.lastError()[0] != '\0';
      break;
    case Screen::Error:
      _screen.error(slot.text, slot.detail);
      refused = !_screen.lastWasPartial() && _screen.lastError()[0] != '\0';
      break;
    case Screen::Count:
      break;
  }

  record.endUs = esp_timer_get_time();
  record.partial = _screen.lastWasPartial();
  record.error = refused ? _screen.lastError() : "";
}
