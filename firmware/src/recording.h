#pragma once

#include <stddef.h>
#include <stdint.h>

#include <atomic>

#include "config.h"

// The recording buffer: one PSRAM allocation that is already a WAV file.
//
// PSRAM makes a ring buffer unnecessary -- there is no memory ceiling to slide
// against -- so this is a linear buffer with a hard cap, and recording stops at
// the cap whether or not the button is still held. At 16 kHz mono 16-bit that
// is 32 KB/s, so the 30 s of config::kMaxRecordSeconds is 960 KB.
//
// **The 44-byte WAV header sits at the front of the allocation**, written once
// by begin() and never again. It declares a length of 0xFFFFFFFF in both of its
// length fields, because from S11 on the header goes up before the recording
// has a length: the body is streamed while the button is held, and the
// terminating chunk is where it ends. The backend takes the length from there
// -- the vision's request contract has why the body stays a WAV, and why that
// value rather than zero. The alternative to reserving the bytes -- capturing
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
// **Two tasks use the buffer at once, and the committed count is what makes
// that safe.** One writes: the capture task, through writeHead(), nextChunkSamples()
// and commit(), and nothing else may call those. The other reads: from S11 the
// orchestrator streams wav() up to wavBytes() while the recording is still
// growing behind it. commit() publishes the count with release ordering and
// every reader of it loads with acquire, so the samples below a count that has
// been read are samples that have landed; and samples never move once they
// have, so nothing past the count needs guarding and nothing below it changes.
// That is why this is a counter rather than a lock -- the writer never waits
// for the reader, which is the one thing the capture task may never do.
//
// begin() and end() are neither side's: they run before the capture task
// starts and after it has been joined.

class Recording {
 public:
  // A canonical PCM WAV header: RIFF/WAVE, one fmt chunk, one data chunk, no
  // extensions. Also a multiple of 4, so the samples behind it stay aligned.
  static constexpr size_t kHeaderBytes = 44;

  // Samples per I2S read during capture. 256 at 16 kHz is 16 ms, which is how
  // often the capture task gets to look at the button -- comfortably inside
  // both the 40 ms release debounce and the 90 ms the I2S DMA holds.
  static constexpr uint32_t kChunkSamples = 256;

  // Allocates the buffer in PSRAM for maxSeconds at sampleRate and writes the
  // header. The rate is the microphone's own -- mic.sampleRate() -- because it
  // both sizes the buffer and goes into the header.
  //
  // False is the vision's NO MEMORY: the one failure where nothing works at
  // all. Calling begin() again with the same shape keeps the allocation and
  // just rewinds.
  bool begin(uint32_t sampleRate, uint32_t maxSeconds = config::kMaxRecordSeconds);

  // Releases the PSRAM.
  void end();

  // The writer's side. Where the next I2S read lands, and how many samples it
  // may write there; the count reaches 0 exactly at the cap, which is what ends
  // capture. Only the task that commits may call these.
  int16_t* writeHead();
  uint32_t nextChunkSamples() const;

  // Publishes the sample count the read actually returned, and with it every
  // sample below it. Anything past the cap is dropped rather than trusted.
  void commit(uint32_t samples);

  // The reader's side, safe from any task: each is one acquire load of the
  // count, so what it reports has landed.
  bool full() const { return _buffer != nullptr && committed() >= _capacity; }
  uint32_t recordedSamples() const { return committed(); }
  size_t recordedBytes() const { return static_cast<size_t>(committed()) * sizeof(int16_t); }
  uint32_t recordedMs() const;

  // The recording as a file, header first: the start of the buffer, and how
  // much of it is there so far. This is the body of the request from S11 on,
  // streamed straight from PSRAM while wavBytes() is still growing, and it is a
  // complete WAV at every length -- the header never declared one.
  const uint8_t* wav() const { return _buffer; }
  size_t wavBytes() const { return kHeaderBytes + recordedBytes(); }

  // Human-readable reason the last begin() returned false.
  const char* lastError() const { return _lastError; }

 private:
  uint32_t committed() const { return _samples.load(std::memory_order_acquire); }

  // Fills the reserved bytes. Called once per allocation; begin() is the only
  // caller.
  void writeHeader();

  uint8_t* _buffer = nullptr;
  uint32_t _capacity = 0;  // samples the buffer holds, header excluded
  std::atomic<uint32_t> _samples{0};
  uint32_t _sampleRate = 0;
  const char* _lastError = "";
};
