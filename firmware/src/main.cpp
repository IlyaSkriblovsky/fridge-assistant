// S4 -- the capture task. A temporary driver, the way S1's, S2's and S3's were:
// docs/implementation.md asks for a recording made while WiFi associates and
// the panel refreshes, with the buffer then checked for gaps. S8 replaces all
// of this with the orchestrator.
//
// This is the step the two-task split exists for, so the driver's job is to put
// the two loads the split is meant to survive on the orchestrator's task and
// leave the recording alone underneath them:
//
//   * a full e-paper refresh, one to two seconds of blocking SPI and a BUSY
//     wait, and
//   * a WiFi association, seconds of an unknown number of them.
//
// Both would eat the I2S DMA's 90 ms several times over if they shared a thread
// with the reads. The WiFi and the panel are raw here rather than modules --
// S5 and S6 are what give them one; what matters at this step is only that
// the orchestrator's task is genuinely busy for seconds on end.
//
// **The ready chirp is back where the vision puts it**: capture first, then the
// chirp, so the chirp honestly means "the microphone is live". S3's driver had
// to chirp before StickyMic::begin() because a 110 ms blocking chirp between
// two reads of a single-task loop tore a hole in the recording. It does not any
// more -- the chirp now sounds on the orchestrator while the capture task keeps
// reading, and the proof is that it is audible in the first blocks of the dump
// with no gap behind it.
//
// One recording per wake, then back to sleep. The press that woke the board is
// the one being timed, so a cold boot -- a flash, or the reset that opening the
// serial monitor causes -- has no button down and stops after the debounce
// window with nothing recorded. Press the AI button and it starts properly.

#include <Arduino.h>
#include <Seeed_GFX.h>
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <esp_timer.h>

#include "config.h"
#include "secrets.h"
#include "sticky_audio.h"
#include "sticky_button.h"
#include "sticky_buzzer.h"
#include "sticky_capture.h"
#include "sticky_epaper.h"
#include "sticky_mic.h"
#include "sticky_power.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// How long the association gets before the driver gives up on it. S6 is where
// this number gets a home in config.h and a measurement behind it; here it only
// has to be long enough not to cut a working connect short.
constexpr uint32_t kWifiTimeoutMs = 15000;

// The dump is meant to be read, so the block size follows the recording: a few
// seconds comes out at 100 ms a row, and a 30 s hold coarsens instead of
// printing three hundred rows.
constexpr uint32_t kMaxDumpRows = 60;
constexpr uint32_t kMinBlockMs = 100;

// The bar spans the range this microphone actually works in: a quiet room reads
// about -70 dBFS on this unit and speech at arm's length peaks near -45 dBFS.
constexpr float kBarFloorDbfs = -80.0f;
constexpr float kBarTopDbfs = -20.0f;
constexpr int kBarWidth = 40;

Seeed_GFX display;
StickyMic mic;
StickyAudio audio;
StickyButton button;
StickyCapture capture;

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
  capture.abort();
  capture.wait();
  mic.end();
  stickyBuzzer::error();
  stickyPower::deepSleep();
}

// The Listening screen, crudely -- S5 is what makes this a module with an
// interface worth keeping. All that is asked of it here is that it is a real
// full refresh on the orchestrator's task while the recording runs.
uint32_t drawListening() {
  const int64_t startUs = esp_timer_get_time();

  if (!display.begin<Board_reTerminal_Sticky, Config_Sticky_SSD1677_Fixed>()) {
    Serial1.printf("  panel: begin failed -- %s\n", display.lastResult().message);
    return 0;
  }

  display.fillScreen(TFT_WHITE);
  display.setTextColor(TFT_BLACK, TFT_WHITE);
  display.setTextFont(1);
  display.setTextSize(6);
  display.setCursor(120, 210);
  display.print("LISTENING");
  display.refresh();

  return static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
}

// Association, polled rather than waited on: WiFi.waitForConnectResult() would
// park the orchestrator inside a call for seconds. S6 is where this becomes a
// module, with the BSSID cached in RTC memory across the sleep.
bool connectWifi(uint32_t& elapsedMs) {
  const int64_t startUs = esp_timer_get_time();
  elapsedMs = 0;

  while (WiFi.status() != WL_CONNECTED) {
    const uint32_t ms = static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
    if (ms > kWifiTimeoutMs) {
      elapsedMs = ms;
      return false;
    }
    delay(10);
  }

  elapsedMs = static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
  return true;
}

// How the run went, as the numbers S4 is checked on. The clock line is the one
// that matters: the task's own wall clock against the audio that came out of
// it. Audio the DMA dropped is time that passed without samples to show for it,
// so a recording that is short of the clock by more than a chunk or two is a
// recording with holes in it.
void reportRun() {
  Serial1.println();
  Serial1.printf("  stop:    %s, button held %lu ms%s\n", capture.stopName(),
                 static_cast<unsigned long>(button.heldMs()),
                 button.released() && button.isTap() ? " (a tap -- discarded)" : "");
  Serial1.printf("  audio:   %lu samples, %lu ms, in %lu chunks of %lu\n",
                 static_cast<unsigned long>(audio.recordedSamples()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(capture.chunks()),
                 static_cast<unsigned long>(StickyAudio::kChunkSamples));

  const int32_t missing =
      static_cast<int32_t>(capture.elapsedMs()) - static_cast<int32_t>(audio.recordedMs());
  Serial1.printf("  clock:   %lu ms of reading, %ld ms of it with no audio to show\n",
                 static_cast<unsigned long>(capture.elapsedMs()),
                 static_cast<long>(missing));
  Serial1.printf("  longest: %.1f ms between two reads, of the 90 ms the DMA holds,\n"
                 "           at chunk %lu; %lu chunks took half again as long as a chunk\n",
                 capture.longestChunkUs() / 1000.0f,
                 static_cast<unsigned long>(capture.longestAtChunk() + 1),
                 static_cast<unsigned long>(capture.slowChunks()));
}

// The dump S3 introduced, over whatever was recorded this time: the buffer
// walked in blocks, each reduced by the same arithmetic a live reading uses.
// Speech has to show up where it was spoken, the chirp in the first blocks --
// and a hole would show up as a discontinuity in the middle of a word.
void reportBlocks() {
  const uint32_t recordedMs = audio.recordedMs();
  if (recordedMs == 0) return;

  uint32_t blockMs = ((recordedMs / kMaxDumpRows) / kMinBlockMs + 1) * kMinBlockMs;
  if (blockMs < kMinBlockMs) blockMs = kMinBlockMs;

  const uint32_t blockSamples = (audio.sampleRate() * blockMs) / 1000;
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
                   static_cast<unsigned long>(index * blockMs), level.rms, level.dbfs,
                   level.peak, bar);
  }

  const MicLevel whole = micLevelOf(samples, total);
  Serial1.println();
  Serial1.printf("  whole recording: %.1f dBFS rms, peak %.0f, loudest block %lu\n",
                 whole.dbfs, whole.peak, static_cast<unsigned long>(loudest + 1));
}

}  // namespace

void setup() {
  // The press started before this line and there is no way to find out how long
  // before: a button wake carries no deadline to measure the boot against. This
  // is the earliest honest origin for the hold.
  const int64_t tEntry = esp_timer_get_time();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(tEntry);

  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  Serial1.println();
  Serial1.println("S4 capture task driver -- one recording per wake, then back to sleep");
  Serial1.printf("  wake %s, reset %s\n", stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());
  Serial1.println("  hold the AI button, speak through the chirp and the refresh, let go");

  // The buffer first: a failure here is the vision's NO MEMORY, and it is worth
  // finding out before the microphone rail is even powered.
  if (!audio.begin(StickyMic::kSampleRate)) fail("audio buffer", audio.lastError());
  Serial1.printf("  buffer: %u bytes of PSRAM, %u free after it\n",
                 static_cast<unsigned>(audio.allocatedBytes()),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
  Serial1.flush();

  // Nothing after this line may block the orchestrator for the microphone's
  // sake: from capture.start() on, the reads are somebody else's problem, which
  // is the whole point of the step.
  if (!mic.begin()) fail("microphone", mic.lastError());
  if (!capture.start(mic, audio, button)) fail("capture task", capture.lastError());

  // Now in the vision's order: the microphone is live, and the chirp says so
  // while the recording is already running.
  stickyBuzzer::ready();

  // Load one, started first because it makes progress in the IDF's own tasks on
  // core 0 while the panel has the orchestrator.
  const bool haveCredentials = secrets::kWifiSsid[0] != '\0';
  if (haveCredentials) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    WiFi.setAutoReconnect(false);
    WiFi.begin(secrets::kWifiSsid, secrets::kWifiPassword);
  } else {
    Serial1.println("  wifi: no credentials in src/secrets.h -- skipping the load");
  }

  // Load two: one to two seconds of SPI and a BUSY wait, on this task.
  const uint32_t panelMs = drawListening();

  uint32_t wifiMs = 0;
  const bool online = haveCredentials && connectWifi(wifiMs);

  if (haveCredentials) {
    if (online) {
      Serial1.printf("  wifi:  associated in %lu ms, %s\n",
                     static_cast<unsigned long>(wifiMs),
                     WiFi.localIP().toString().c_str());
    } else {
      // The vision's rule, exercised: a recording with nowhere to go is stopped
      // rather than finished.
      Serial1.printf("  wifi:  gave up after %lu ms -- aborting the recording\n",
                     static_cast<unsigned long>(wifiMs));
      capture.abort();
    }
  }
  Serial1.printf("  panel: full refresh in %lu ms\n", static_cast<unsigned long>(panelMs));
  Serial1.println("  waiting for the button...");
  Serial1.flush();

  // Both loads are over; from here the orchestrator has nothing to do but wait
  // for the release, which the capture task is what notices.
  capture.wait();
  mic.end();

  // The sounds the flow would make, so the run can be followed without the
  // cable: an abort and a dead microphone are both errors, the cap is a
  // question like any other, and a tap gets nothing at all -- D7.
  switch (capture.stop()) {
    case StickyCapture::Stop::Aborted:
    case StickyCapture::Stop::ReadFailed:
      stickyBuzzer::error();
      break;
    case StickyCapture::Stop::Released:
      if (!button.isTap()) stickyBuzzer::answer();
      break;
    default:
      stickyBuzzer::answer();
      break;
  }

  reportRun();
  if (capture.stop() == StickyCapture::Stop::ReadFailed) {
    Serial1.printf("  microphone: %s\n", capture.lastError());
  }
  reportBlocks();

  Serial1.flush();
  stickyPower::deepSleep();
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
