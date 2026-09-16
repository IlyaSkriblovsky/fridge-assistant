#include "sticky_audio.h"

#include <esp_heap_caps.h>
#include <string.h>

namespace {

// A cursor over the reserved header bytes. The ESP32-S3 is little-endian and a
// struct with a memcpy would work, but a WAV header is defined field by field
// and writing it that way keeps each field's width on the page next to it.
struct HeaderWriter {
  uint8_t* p;

  void tag(const char* fourCC) {
    memcpy(p, fourCC, 4);
    p += 4;
  }
  void u16(uint16_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p += 2;
  }
  void u32(uint32_t v) {
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
    p += 4;
  }
};

}  // namespace

bool StickyAudio::begin(uint32_t sampleRate, uint32_t maxSeconds) {
  if (sampleRate == 0 || maxSeconds == 0) {
    _lastError = "zero-length buffer requested";
    return false;
  }

  const uint32_t capacity = sampleRate * maxSeconds;
  if (_buffer != nullptr) {
    if (capacity == _capacity && sampleRate == _sampleRate) {
      reset();
      return true;
    }
    end();
  }

  const size_t bytes = kHeaderBytes + static_cast<size_t>(capacity) * sizeof(int16_t);
  _buffer = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
  if (_buffer == nullptr) {
    _lastError = "PSRAM allocation failed";
    return false;
  }

  _capacity = capacity;
  _sampleRate = sampleRate;
  _samples = 0;
  _lastError = "";
  return true;
}

void StickyAudio::end() {
  if (_buffer != nullptr) {
    heap_caps_free(_buffer);
    _buffer = nullptr;
  }
  _capacity = 0;
  _samples = 0;
  _sampleRate = 0;
}

int16_t* StickyAudio::writeHead() {
  if (_buffer == nullptr) return nullptr;
  return reinterpret_cast<int16_t*>(_buffer + kHeaderBytes) + _samples;
}

uint32_t StickyAudio::nextChunkSamples() const {
  if (_buffer == nullptr) return 0;
  const uint32_t room = _capacity - _samples;
  return room < kChunkSamples ? room : kChunkSamples;
}

void StickyAudio::commit(uint32_t samples) {
  if (_buffer == nullptr) return;
  const uint32_t room = _capacity - _samples;
  _samples += samples < room ? samples : room;
}

uint32_t StickyAudio::recordedMs() const {
  if (_sampleRate == 0) return 0;
  return static_cast<uint32_t>(static_cast<uint64_t>(_samples) * 1000 / _sampleRate);
}

const int16_t* StickyAudio::samples() const {
  if (_buffer == nullptr) return nullptr;
  return reinterpret_cast<const int16_t*>(_buffer + kHeaderBytes);
}

size_t StickyAudio::allocatedBytes() const {
  if (_buffer == nullptr) return 0;
  return kHeaderBytes + static_cast<size_t>(_capacity) * sizeof(int16_t);
}

const uint8_t* StickyAudio::wav() {
  if (_buffer == nullptr) return nullptr;

  // Mono 16-bit, which is what the PDM path delivers and what the vision's
  // request contract promises the backend.
  constexpr uint16_t kChannels = 1;
  constexpr uint16_t kBitsPerSample = 16;
  constexpr uint16_t kBlockAlign = kChannels * kBitsPerSample / 8;

  const uint32_t dataBytes = static_cast<uint32_t>(recordedBytes());

  HeaderWriter w{_buffer};
  w.tag("RIFF");
  w.u32(36 + dataBytes);  // everything in the file after this field
  w.tag("WAVE");
  w.tag("fmt ");
  w.u32(16);              // fmt chunk body length: 16 for plain PCM
  w.u16(1);               // format tag: PCM, uncompressed
  w.u16(kChannels);
  w.u32(_sampleRate);
  w.u32(_sampleRate * kBlockAlign);  // byte rate
  w.u16(kBlockAlign);
  w.u16(kBitsPerSample);
  w.tag("data");
  w.u32(dataBytes);

  return _buffer;
}
