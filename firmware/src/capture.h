#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <stdint.h>

class Recording;
class StickyButton;
class StickyMic;

// The recording, as a task of its own -- the one thing on this device that is
// never allowed to wait for anything.
//
// The I2S DMA holds 6 x 240 frames, which is 90 ms at 16 kHz. Whatever else the
// firmware is doing, a read has to come back inside that window or the ring
// overflows and the audio that arrived meanwhile is gone. A WiFi association is
// seconds and a full e-paper refresh is one to two, so neither can share a
// thread with the reads; that is the whole reason this class exists, and the
// vision's concurrency decision in one sentence.
//
// **The button poll lives here too**, between reads. A gpio_get_level() costs
// nothing and never blocks, and it buys a tight stop: the orchestrator polls in
// a loop that also refreshes the panel, so a release arriving during a refresh
// would otherwise add a second or two of room noise to the end of every
// recording. Polled every chunk -- 256 samples, 16 ms -- which is comfortably
// inside the 40 ms release debounce, so the release is seen 40-56 ms after the
// line goes high and not later.
//
// Ownership is handed over rather than shared, so nothing here is locked. From
// start() until the task reports finished(), the microphone, the buffer and the
// button belong to the task and the orchestrator must not touch them; after
// that they are the orchestrator's again. The two flags that cross the boundary
// while the task runs are the abort request going in and the finish coming out,
// and both go through primitives that carry the barriers with them.
//
// Nothing in the task prints. Serial1 is the orchestrator's, and a line of log
// at 115200 is a millisecond that the reads do not have to spare.
class Capture {
 public:
  // Core 1 is where Arduino's loopTask runs and where there is nothing else:
  // the WiFi and lwIP tasks are pinned to core 0, and that split is what makes
  // "recording while associating" cost nothing at all rather than cost
  // context switches.
  static constexpr BaseType_t kCore = 1;

  // Above loopTask, which runs at 1, so a DMA buffer coming ready preempts the
  // orchestrator instead of queueing behind a panel refresh. Well below lwIP
  // (18) and WiFi (23), which sit on the other core anyway.
  static constexpr UBaseType_t kPriority = 10;

  // The task calls readSamples() and poll() and nothing else -- no printing, no
  // formatting, no library that allocates. 4 KB is already generous.
  static constexpr uint32_t kStackBytes = 4096;

  // Why the recording ended. Released and Full are both normal questions --
  // the vision counts the cap as a question that was simply long enough.
  enum class Stop : uint8_t {
    None,        // still running, or never started
    Released,    // the button came back up and stayed up
    Full,        // the 30 s cap
    Aborted,     // the orchestrator called abort(): WiFi went away
    ReadFailed,  // the microphone stopped delivering
  };

  ~Capture();

  // Starts recording into `audio` from `mic`, stopping when `button` reports a
  // debounced release. The microphone must already be up: begin() powers a rail
  // and waits out a settle window, which is exactly the kind of blocking this
  // task exists to keep out of the timeline.
  //
  // Returns false only if the task could not be created, which leaves nothing
  // running and nothing recorded.
  bool start(StickyMic& mic, Recording& audio, StickyButton& button);

  // Asks the task to stop and returns immediately -- the vision's "a network
  // failure during recording aborts rather than letting the user finish talking
  // into a recording that has nowhere to go". The task notices between two
  // reads, so the recording ends within a chunk. Wait for finished() before
  // touching the buffer.
  void abort();

  // Non-blocking: true once the task is over and everything it wrote is the
  // caller's to read. Latches, so it keeps returning true.
  bool finished();

  // finished(), but blocking up to timeoutMs. Returns false on the timeout,
  // which means the task is still running.
  bool wait(uint32_t timeoutMs = UINT32_MAX);

  // Meaningful once finished() is true.
  Stop stop() const { return _stop; }
  const char* stopName() const;

  // Why the microphone stopped, when stop() is ReadFailed.
  const char* lastError() const { return _lastError; }

  // What the run cost, for the log. The wall clock the task spent reading is
  // the drop detector: audio that was captured is Recording::recordedMs(),
  // and anything the clock has beyond it is audio the DMA threw away while
  // nobody was reading. They should differ by a chunk or two, no more.
  uint32_t elapsedMs() const { return static_cast<uint32_t>(_elapsedUs / 1000); }
  uint32_t chunks() const { return _chunks; }

  // The longest gap between two consecutive reads returning, and which chunk it
  // was. It is not the headroom and should not be read against the DMA's 90 ms:
  // the gap counts the blocking read itself, and a read that waits is waiting
  // inside the DMA rather than away from it. What costs audio is time spent
  // away from the read, and elapsedMs() against recordedMs() is what measures
  // that. Steady state on this board is 15 ms, with every fifteenth read at
  // 30 ms because a 256-sample chunk has to straddle two 240-frame blocks.
  uint32_t longestChunkUs() const { return _longestChunkUs; }
  uint32_t longestAtChunk() const { return _longestAtChunk; }

  // How many chunks took half again as long as they should have. One number
  // that says where the slow reads come from: scattered evenly through the run
  // they are the DMA's own granularity, while a handful of them is something
  // the orchestrator did.
  uint32_t slowChunks() const { return _slowChunks; }

 private:
  static void trampoline(void* self);
  void run();
  bool join(TickType_t ticks);

  StickyMic* _mic = nullptr;
  Recording* _audio = nullptr;
  StickyButton* _button = nullptr;

  TaskHandle_t _task = nullptr;
  SemaphoreHandle_t _done = nullptr;
  volatile bool _abort = false;
  bool _joined = false;

  Stop _stop = Stop::None;
  const char* _lastError = "";
  int64_t _elapsedUs = 0;
  uint32_t _chunks = 0;
  uint32_t _longestChunkUs = 0;
  uint32_t _longestAtChunk = 0;
  uint32_t _slowChunks = 0;
};
