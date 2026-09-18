#include "capture.h"

#include <esp_timer.h>

#include "sticky/button.h"
#include "sticky/mic.h"

#include "recording.h"

Capture::~Capture() {
  if (_task != nullptr) {
    abort();
    wait();
  }
  if (_done != nullptr) vSemaphoreDelete(_done);
}

bool Capture::start(StickyMic& mic, Recording& audio, StickyButton& button) {
  if (_task != nullptr) {
    _lastError = "capture already running";
    return false;
  }
  if (_done == nullptr) {
    _done = xSemaphoreCreateBinary();
    if (_done == nullptr) {
      _lastError = "no memory for the done semaphore";
      return false;
    }
  }

  _mic = &mic;
  _audio = &audio;
  _button = &button;

  _abort = false;
  _joined = false;
  _stopReason = StopReason::None;
  _lastError = "";
  _elapsedUs = 0;
  _chunks = 0;
  _longestChunkUs = 0;
  _longestAtChunk = 0;
  _slowChunks = 0;

  if (xTaskCreatePinnedToCore(trampoline, "capture", kStackBytes, this, kPriority, &_task,
                              kCore) != pdPASS) {
    _task = nullptr;
    _lastError = "capture task could not be created";
    return false;
  }
  return true;
}

void Capture::abort() { _abort = true; }

bool Capture::finished() { return join(0); }

bool Capture::wait(uint32_t timeoutMs) {
  return join(timeoutMs == UINT32_MAX ? portMAX_DELAY : pdMS_TO_TICKS(timeoutMs));
}

bool Capture::join(TickType_t ticks) {
  if (_joined) return true;
  if (_done == nullptr) return true;  // never started: nothing to wait for
  if (xSemaphoreTake(_done, ticks) != pdTRUE) return false;

  // The give on the other side is a full barrier, so everything the task wrote
  // before it is visible here. From this line on the microphone, the buffer and
  // the button are the caller's again.
  _joined = true;
  _task = nullptr;
  return true;
}

const char* Capture::stopReasonName() const {
  switch (_stopReason) {
    case StopReason::Released: return "released";
    case StopReason::Full: return "cap reached";
    case StopReason::Aborted: return "aborted";
    case StopReason::ReadFailed: return "read failed";
    case StopReason::None: break;
  }
  return "still running";
}

void Capture::trampoline(void* self) { static_cast<Capture*>(self)->run(); }

void Capture::run() {
  const int64_t startUs = esp_timer_get_time();
  int64_t previousUs = startUs;

  // What a chunk is worth in wall clock, and the line above which a read counts
  // as having waited for something. Half again as long is well clear of the
  // jitter a 240-frame DMA block puts on a 256-sample read and well below the
  // 90 ms at which audio actually starts going missing.
  const uint32_t nominalUs =
      static_cast<uint32_t>(Recording::kChunkSamples * 1000000ULL / _mic->sampleRate());
  const uint32_t slowUs = nominalUs + nominalUs / 2;

  for (;;) {
    // Checked first, so an abort that arrives while a read is in flight costs
    // one chunk and not two.
    if (_abort) {
      _stopReason = StopReason::Aborted;
      break;
    }

    const uint32_t want = _audio->nextChunkSamples();
    if (want == 0) {
      _stopReason = StopReason::Full;  // the 30 s cap, which is a question like any other
      break;
    }

    // Straight into the PSRAM buffer at the write head: no intermediate chunk,
    // no copy afterwards. Blocks for the chunk's own duration, 16 ms, which is
    // where this task spends essentially all of its time.
    const uint32_t got = _mic->readSamples(_audio->writeHead(), want);
    if (got == 0) {
      _lastError = _mic->lastError();
      _stopReason = StopReason::ReadFailed;
      break;
    }
    _audio->commit(got);

    const int64_t now = esp_timer_get_time();
    const uint32_t sinceLast = static_cast<uint32_t>(now - previousUs);
    if (sinceLast > _longestChunkUs) {
      _longestChunkUs = sinceLast;
      _longestAtChunk = _chunks;
    }
    if (sinceLast > slowUs) ++_slowChunks;
    previousUs = now;
    ++_chunks;

    if (_button->poll()) {
      _stopReason = StopReason::Released;
      break;
    }
  }

  _elapsedUs = esp_timer_get_time() - startUs;

  // The last thing this task does with the object. xSemaphoreGive() carries the
  // barrier that makes everything above visible to whoever comes back out of
  // finished() or wait(), and the caller is free to reuse or destroy the object
  // the moment it returns -- so nothing below this line may touch `this`.
  SemaphoreHandle_t done = _done;
  xSemaphoreGive(done);
  vTaskDelete(nullptr);
}
