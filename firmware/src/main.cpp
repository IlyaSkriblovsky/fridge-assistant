// S3 -- capture into PSRAM. A temporary driver, the way S1's and S2's were:
// docs/implementation.md asks for a fixed few seconds recorded and the levels
// of the finished buffer logged block by block, so that speech has to show up
// where it was spoken. S8 replaces all of this with the orchestrator.
//
// One recording per wake, then back to sleep. The board has no off switch, so
// nothing here repeats on its own: press the AI button, wait for the ready
// chirp, say something at a moment you can find again in the dump -- two words,
// a pause, two words -- and the answer chirp says the recording is over.
//
// **The chirp comes before capture here, which is the opposite of the flow.**
// The vision starts capture first so that the chirp can honestly mean "the
// microphone is live", but that only works once the chirp is on another task:
// it blocks for 110 ms, and the I2S DMA holds 90 ms, so a chirp between two
// reads of a single-task loop overflows the ring and tears a hole in the
// recording. Chirping before StickyMic::begin() costs the user the 34 ms of
// rail and settle that follow, and leaves the timeline in the dump exact --
// which is the thing this step is checked on. S4 is where the order in the
// vision becomes free.
//
// The button is not read at all here. The recording is a fixed length because
// this step is about the buffer; the press only supplies the wake, and S4 is
// where the length becomes the user's again.

#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_memory_utils.h>

#include "config.h"
#include "sticky_audio.h"
#include "sticky_buzzer.h"
#include "sticky_mic.h"
#include "sticky_power.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// Long enough to fit a phrase, a silence and a phrase, short enough that the
// dump is a screen and not a scroll.
constexpr uint32_t kRecordMs = 5000;

// One row per block of the dump. 100 ms is finer than a syllable, so a word
// lands in several rows and a pause is unmistakable.
constexpr uint32_t kBlockMs = 100;

// The bar spans the range this microphone actually works in: a quiet room reads
// about -70 dBFS on this unit and speech at arm's length peaks near -45 dBFS.
constexpr float kBarFloorDbfs = -80.0f;
constexpr float kBarTopDbfs = -20.0f;
constexpr int kBarWidth = 40;

StickyMic mic;
StickyAudio audio;

// Fills buf with a bar of kBarWidth cells, proportional to dbfs. Reading fifty
// rows of numbers for the one place the speech is takes longer than looking.
void drawBar(char* buf, size_t size, float dbfs) {
  float t = (dbfs - kBarFloorDbfs) / (kBarTopDbfs - kBarFloorDbfs);
  if (t < 0.0f) t = 0.0f;
  if (t > 1.0f) t = 1.0f;

  int filled = static_cast<int>(t * kBarWidth + 0.5f);
  size_t n = 0;
  while (n + 1 < size && n < static_cast<size_t>(filled)) buf[n++] = '#';
  buf[n] = '\0';
}

// Everything that goes wrong here ends the same way: say what it was, sound the
// error pattern and sleep. On battery the chirp is the whole message.
[[noreturn]] void fail(const char* what, const char* detail) {
  Serial1.printf("FAILED: %s -- %s\n", what, detail);
  Serial1.flush();
  mic.end();
  stickyBuzzer::error();
  stickyPower::deepSleep();
}

void reportBuffer() {
  Serial1.printf("  buffer: %u bytes at %p (%s), %lu s at %lu Hz\n",
                 static_cast<unsigned>(audio.allocatedBytes()),
                 static_cast<const void*>(audio.samples()),
                 esp_ptr_external_ram(audio.samples()) ? "PSRAM" : "NOT PSRAM",
                 static_cast<unsigned long>(config::kMaxRecordSeconds),
                 static_cast<unsigned long>(audio.sampleRate()));
  Serial1.printf("  PSRAM free after it: %u of %u bytes\n",
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_total_size(MALLOC_CAP_SPIRAM)));
}

// The header, once, as bytes. Until S7 exists there is nothing else that would
// notice a wrong field, and a WAV a player refuses is a long way to find a
// swapped byte order.
void reportHeader() {
  const uint8_t* wav = audio.wav();
  Serial1.printf("  wav: %u bytes total, %u of header\n",
                 static_cast<unsigned>(audio.wavBytes()),
                 static_cast<unsigned>(StickyAudio::kHeaderBytes));
  for (size_t i = 0; i < StickyAudio::kHeaderBytes; i += 16) {
    Serial1.printf("   %02u ", static_cast<unsigned>(i));
    for (size_t j = i; j < i + 16 && j < StickyAudio::kHeaderBytes; ++j) {
      Serial1.printf("%02x ", wav[j]);
    }
    Serial1.println();
  }
}

// The dump the step is verified on: the recorded buffer walked in blocks, each
// reduced by the same arithmetic a live reading uses.
void reportBlocks() {
  const uint32_t blockSamples = (audio.sampleRate() * kBlockMs) / 1000;
  if (blockSamples == 0) return;

  Serial1.println();
  Serial1.printf("  %5s %7s %9s %8s %7s  %s\n", "block", "at", "rms", "dBFS", "peak",
                 "level");

  const int16_t* samples = audio.samples();
  const uint32_t total = audio.recordedSamples();
  uint32_t loudest = 0;
  float loudestDbfs = StickyMic::kSilenceDbfs;

  for (uint32_t start = 0, index = 0; start < total; start += blockSamples, ++index) {
    uint32_t count = total - start;
    if (count > blockSamples) count = blockSamples;

    const MicLevel level = micLevelOf(samples + start, count);
    if (level.dbfs > loudestDbfs) {
      loudestDbfs = level.dbfs;
      loudest = index;
    }

    char bar[kBarWidth + 1];
    drawBar(bar, sizeof(bar), level.dbfs);
    Serial1.printf("  %5lu %6lu %9.1f %8.1f %7.0f  %s\n",
                   static_cast<unsigned long>(index + 1),
                   static_cast<unsigned long>(index * kBlockMs), level.rms, level.dbfs,
                   level.peak, bar);
  }

  const MicLevel whole = micLevelOf(samples, total);
  Serial1.println();
  Serial1.printf("  whole recording: %.1f dBFS rms, peak %.0f, loudest block %lu\n",
                 whole.dbfs, whole.peak, static_cast<unsigned long>(loudest + 1));
}

}  // namespace

void setup() {
  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();

  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  Serial1.println();
  Serial1.println("S3 capture driver -- one recording per wake, then back to sleep");
  Serial1.printf("  wake %s, reset %s\n", stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());

  // The buffer first: a failure here is the vision's NO MEMORY, and it is worth
  // finding out before the microphone rail is even powered.
  if (!audio.begin(StickyMic::kSampleRate)) fail("audio buffer", audio.lastError());
  reportBuffer();
  Serial1.printf("  recording %lu ms from the ready chirp -- speak after it\n",
                 static_cast<unsigned long>(kRecordMs));
  Serial1.flush();

  // Then the chirp, and only then the microphone -- see the note at the top of
  // the file. Nothing may block between begin() and the loop for longer than
  // the DMA holds, which is why the printing above is already done.
  stickyBuzzer::ready();
  if (!mic.begin()) fail("microphone", mic.lastError());

  const uint32_t wanted =
      static_cast<uint32_t>(static_cast<uint64_t>(mic.sampleRate()) * kRecordMs / 1000);

  while (audio.recordedSamples() < wanted) {
    uint32_t want = audio.nextChunkSamples();
    if (want == 0) break;  // the 30 s cap, which kRecordMs is well inside
    const uint32_t remaining = wanted - audio.recordedSamples();
    if (want > remaining) want = remaining;

    const uint32_t got = mic.readSamples(audio.writeHead(), want);
    if (got == 0) fail("capture", mic.lastError());
    audio.commit(got);
  }

  mic.end();
  stickyBuzzer::answer();

  Serial1.printf("  recorded %lu samples, %lu ms\n",
                 static_cast<unsigned long>(audio.recordedSamples()),
                 static_cast<unsigned long>(audio.recordedMs()));
  reportHeader();
  reportBlocks();

  Serial1.flush();
  stickyPower::deepSleep();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
