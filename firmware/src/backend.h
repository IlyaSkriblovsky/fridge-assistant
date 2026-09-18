#pragma once

#include <stddef.h>
#include <stdint.h>

// The question, as one HTTP round trip: the recording goes up, the answer comes
// back.
//
// The contract is the vision's and it is deliberately small --
//
//     POST {config::kBackendBaseUrl}{config::kAudioPath}
//     Content-Type: audio/wav
//     <WAV: PCM, 16 kHz, mono, signed 16-bit little-endian>
//
//     200 OK
//     {"response": "..."}
//
// -- so the body is a file the backend can save and play back, with the sample
// rate and the format in the WAV header rather than in headers of our own. It
// carries no credentials (D8) and goes over plain HTTP (D5).
//
// **The body is sent from PSRAM by pointer and length.** Recording reserved
// the 44 header bytes at the front of its allocation, so what goes on the wire
// is that allocation itself and nothing copies a megabyte. It is also why
// Arduino's own HTTPClient is enough here: it can send a body only when the
// length is known in advance, which is exactly the body this step has. D4 --
// the streaming upload that opens the request on the press -- is the day that
// stops being true and esp_http_client takes over.
//
// **Everything that can go wrong is four screens**, which is the upload half of
// the vision's error table. The distinctions are the ones the user can act on:
// nothing at the address (NO SERVER), something there that refused (SERVER
// ERROR), something there that answered with a body that is not an answer (BAD
// RESPONSE), and something that took the question and never came back (TIMED
// OUT). Anything finer belongs in the log, which is what lastError() is for --
// the same division WifiLink draws between NO WIFI and its own strings.
//
// **ask() blocks for the whole round trip**, up to config::kResponseTimeoutMs,
// and that is allowed here and nowhere else in the firmware: by the time it is
// called the button is up, the recording is over and the capture task has been
// joined, so there is nothing left for this thread to be late for.
class Backend {
 public:
  // How the round trip ended. The orchestrator turns these into the vision's
  // screens; nothing here knows what they are called.
  enum class Result : uint8_t {
    Ok,           // answer() holds the text
    NoServer,     // nothing accepted a connection, or one died mid-request
    ServerError,  // answered, but not with 200 -- status() has the code
    BadResponse,  // 200, and a body that is not {"response": "..."}
    TimedOut,     // connected, then silence for the whole budget
  };

  // The answer is kept in place rather than on the heap, so that nothing
  // downstream depends on the parser's lifetime. StickyScreen truncates at its
  // own kMaxTextChars and D2 means anything past the first line is off the
  // right edge anyway, so this bound is only ever reached by a backend that is
  // answering with something other than an answer -- it is generous enough that
  // the log shows more than the panel does.
  static constexpr size_t kMaxAnswerChars = 256;

  // config::kBackendBaseUrl plus config::kAudioPath, comfortably.
  static constexpr size_t kMaxUrlChars = 96;

  // A reply longer than this is not an answer. Without a ceiling a backend that
  // went wrong could have the device read and parse a megabyte on the heap, for
  // a string the panel cuts at 128 characters.
  static constexpr size_t kMaxBodyBytes = 4096;

  // The question. Returns how it ended; everything below narrows that down.
  Result ask(const uint8_t* wav, size_t bytes);

  // The same, somewhere else: the seam that provokes each row of the vision's
  // error table -- a closed port, an address nothing answers at, a backend that
  // returns 500 -- without a rebuild between rows. Nothing in the firmware calls
  // it. S7's driver did, and S11 needs it again: the four results get mapped
  // onto esp_http_client there, and each row has to be walked once more.
  Result ask(const char* url, const uint8_t* wav, size_t bytes);

  // Meaningful after Ok. Always a valid string: empty before the first ask().
  const char* answer() const { return _answer; }

  // The HTTP status, once there was one. Zero when the request never got that
  // far, which is every NoServer and every TimedOut.
  int status() const { return _status; }

  // True when the round trip ended because nothing at all answered at the
  // address -- a connect that ran out of its own budget rather than one that
  // was refused. It is the one distinction inside NoServer that the device can
  // act on, and S7b is what acts on it: a refusal proves something is at the
  // address and therefore that the address works, while silence is also the
  // shape of a cached DHCP lease that has outlived its network. HTTPClient
  // reports both as "connection refused", so the clock is what separates them.
  bool unreachable() const { return _unreachable; }

  // What the reply weighed, for the log.
  size_t bodyBytes() const { return _bodyBytes; }

  // ask() to the answer in hand, and ask() to the first byte of the response.
  // The gap between them is the body, which for this contract is nothing; the
  // second number is the upload plus whatever the backend spent thinking, and
  // it is the one E2 compares between http:// and https://.
  uint32_t elapsedMs() const { return _elapsedMs; }
  uint32_t firstByteMs() const { return _firstByteMs; }

  // Why it ended that way, for Serial1. The screen gets one of four titles
  // instead; this is the string that says which of the ways it was.
  const char* lastError() const { return _error; }

 private:
  // The configured endpoint, written in one place so nothing else concatenates
  // it.
  static void endpoint(char* out, size_t size);

  // One exit, so no path can return without stopping the clock.
  Result finish(Result result, const char* format, ...);

  int64_t _startUs = 0;
  uint32_t _elapsedMs = 0;
  uint32_t _firstByteMs = 0;
  int _status = 0;
  size_t _bodyBytes = 0;
  bool _unreachable = false;

  char _answer[kMaxAnswerChars] = {0};
  char _error[80] = {0};
};
