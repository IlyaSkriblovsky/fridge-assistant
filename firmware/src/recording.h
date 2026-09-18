#pragma once

#include <stddef.h>
#include <stdint.h>

#include "config.h"

// The recording buffer: one PSRAM allocation that is already a WAV file.
//
// PSRAM makes a ring buffer unnecessary -- there is no memory ceiling to slide
// against -- so this is a linear buffer with a hard cap, and recording stops at
// the cap whether or not the button is still held. At 16 kHz mono 16-bit that
// is 32 KB/s, so the 30 s of config::kMaxRecordSeconds is 960 KB.
//
// **The 44-byte WAV header is reserved at the front of the allocation** and
// filled in by wav() once the length is known. The alternative -- capturing
// into a bare PCM buffer and prepending a header at send time -- copies a
// megabyte for 44 bytes.
//
// Nothing here copies audio either: writeHead() hands out the address inside
// the buffer that the next I2S read should land at, so samples go from the DMA
// to their final home in one step. The sequence is
//
//     const uint32_t want = audio.nextChunkSamples();      // 0 at the cap
//     const uint32_t got = mic.readSamples(audio.writeHead(), want);
//     audio.commit(got);
//
// which from S4 on is the body of the capture task's loop, with the button
// poll between two passes of it.
//
// The samples keep the microphone's DC bias: the ESP32-S3's PDM-to-PCM path has
// no high-pass stage, and removing the bias here would be a second pass over a
// megabyte for something the backend can do while it decodes. See E3 in
// docs/experiments.md, which is the same subject.
//
// One task owns the object at a time. S4 hands it to the capture task and takes
// it back on release rather than sharing it, so there is no locking here.

class Recording {
 public:
  // A canonical PCM WAV header: RIFF/WAVE, one fmt chunk, one data chunk, no
  // extensions. Also a multiple of 4, so the samples behind it stay aligned.
  static constexpr size_t kHeaderBytes = 44;

  // Samples per I2S read during capture. 256 at 16 kHz is 16 ms, which is how
  // often the capture task gets to look at the button -- comfortably inside
  // both the 40 ms release debounce and the 90 ms the I2S DMA holds.
  static constexpr uint32_t kChunkSamples = 256;

  // Allocates the buffer in PSRAM for maxSeconds at sampleRate. The rate is
  // the microphone's own -- mic.sampleRate() -- because it both sizes the
  // buffer and goes into the header.
  //
  // False is the vision's NO MEMORY: the one failure where nothing works at
  // all. Calling begin() again with the same shape keeps the allocation and
  // just rewinds.
  bool begin(uint32_t sampleRate, uint32_t maxSeconds = config::kMaxRecordSeconds);

  // Releases the PSRAM.
  void end();

  // Where the next I2S read lands, and how many samples it may write there.
  // The count reaches 0 exactly at the cap, which is what ends capture.
  int16_t* writeHead();
  uint32_t nextChunkSamples() const;

  // Takes the sample count the read actually returned. Anything past the cap
  // is dropped rather than trusted.
  void commit(uint32_t samples);

  bool full() const { return _buffer != nullptr && _samples >= _capacity; }
  uint32_t recordedSamples() const { return _samples; }
  size_t recordedBytes() const { return static_cast<size_t>(_samples) * sizeof(int16_t); }
  uint32_t recordedMs() const;

  // The recording in place, for anything that wants to read it without a copy.
  const int16_t* samples() const;

  // Writes the header over the reserved bytes and returns the start of the
  // buffer. This pointer and wavBytes() are the POST body, unchanged from S7
  // on: the body is sent straight from PSRAM.
  const uint8_t* wav();
  size_t wavBytes() const { return kHeaderBytes + recordedBytes(); }

  // Human-readable reason the last begin() returned false.
  const char* lastError() const { return _lastError; }

 private:
  uint8_t* _buffer = nullptr;
  uint32_t _capacity = 0;  // samples the buffer holds, header excluded
  uint32_t _samples = 0;
  uint32_t _sampleRate = 0;
  const char* _lastError = "";
};
