#pragma once

#include <stddef.h>
#include <stdint.h>

// esp_http_client.h stays in the .cpp; this is its handle type.
struct esp_http_client;

// The question, as one HTTP request that opens while the button is still held:
// the recording goes up as it is made, the answer comes back.
//
// The contract is the vision's and it is deliberately small --
//
//     POST {base URL}{config::kAudioPath}
//     Authorization: Bearer <token>
//     Content-Type: audio/wav
//     Transfer-Encoding: chunked
//     <WAV: PCM, 16 kHz, mono, signed 16-bit little-endian>
//
//     200 OK
//     {"response": "..."}
//
// -- so the body is a file the backend can save and play back, with the sample
// rate and the format in the WAV header rather than in headers of our own. The
// header's two length fields say 0xFFFFFFFF, because it goes up before the
// recording has a length; the terminating chunk is where the body ends, and the
// backend takes the length from there. The token is the device's one
// credential, a static string from src/secrets.h that the backend holds too, and
// it goes over plain HTTP (D5): anyone on the path can read it.
//
// **The request is four calls, because the body is a stream:**
// open() while the button is held, write() as the capture task commits, end()
// once it has stopped, receive() for the answer. The orchestrator's thread makes
// all four, and every one of them blocks it -- which is allowed here because
// nothing else on that thread has a deadline: the release is timed by the
// capture task, and the buffer is linear, so a write that sits through one of
// E7's stalls has nothing to overrun.
//
// **esp_http_client, not Arduino's HTTPClient**, which sends only bodies whose
// length it knows. esp_http_client_open() with a negative length announces a
// chunked body and then leaves the chunks to the caller: esp_http_client_write()
// puts bytes on the socket exactly as given, framing included, so the framing
// is written here.
//
// **Each chunk leaves as one write**, framed in a small buffer: the size line,
// up to kFrameBytes of samples, the CRLF. Three writes would do the same job
// with the samples straight from PSRAM, but the socket has Nagle's algorithm
// switched off -- the terminating chunk must not wait for the backend to get
// round to acknowledging the chunk before it -- and with nothing coalescing
// them each of the three would be a packet of its own.
//
// **Everything that can go wrong is four screens**, which is the upload half of
// the vision's error table. The distinctions are the ones the user can act on:
// nothing at the address (NO SERVER), something there that refused (SERVER
// ERROR), something there that answered with a body that is not an answer (BAD
// RESPONSE), and something that took the question and never came back (TIMED
// OUT). Anything finer belongs in the log, which is what lastError() is for --
// the same division WifiLink draws between NO WIFI and its own strings.
class Backend {
 public:
  // How a call ended. The orchestrator turns these into the vision's screens;
  // nothing here knows what they are called.
  enum class Result : uint8_t {
    Ok,           // so far, or answer() holds the text
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

  // The base URL plus config::kAudioPath, comfortably.
  static constexpr size_t kMaxUrlChars = 96;

  // A reply longer than this is not an answer. Without a ceiling a backend that
  // went wrong could have the device read and parse a megabyte, for a string
  // the panel cuts at 128 characters. The reply is read into a buffer of this
  // size that lives in the object, so there is no allocation to fail.
  static constexpr size_t kMaxReplyBytes = 4096;

  // The most samples one chunk carries. Under the hold a chunk is whatever the
  // capture task committed since the last poll, 512 bytes most of the time, so
  // this only bounds the backlog -- the audio recorded before the request
  // opened, or during a stall -- and the buffer it is framed in.
  static constexpr size_t kFrameBytes = 2048;

  // Both come from src/secrets.h and have to outlive the Backend, since they
  // are kept by pointer; its string constants do. `baseUrl` is the backend
  // without the path, and an empty one fails every open() as NoServer, saying
  // why. `token` goes up with every request as `Authorization: Bearer`; an
  // empty one is not sent at all, and the backend's 401 is how that shows.
  Backend(const char* baseUrl, const char* token);
  ~Backend();

  // Connects and sends the request line and headers, within
  // config::kBackendConnectTimeoutMs. True means the request is open and
  // write() may follow. On false the connection is gone and result(),
  // unreachable() and lastError() say how.
  //
  // Opening again drops whatever request was open and starts a new one from
  // byte zero, which is what a retry is.
  bool open();

  // The same, somewhere else: the seam that provokes each row of the vision's
  // error table -- a closed port, an address nothing answers at, a backend that
  // returns 500 -- without editing the endpoint. Nothing in the firmware calls
  // it.
  bool open(const char* url);

  // Sends `bytes` of the body as chunks, blocking until the socket has taken
  // them. A connection that takes nothing for config::kBackendStallTimeoutMs has
  // failed. On false the connection is gone and result() says how.
  bool write(const uint8_t* data, size_t bytes);

  // The terminating chunk: the body is over. From here the backend has
  // config::kResponseTimeoutMs to answer.
  bool end();

  // Waits for the answer and reads it. Returns how the question ended; after
  // Ok, answer() holds the text. The connection is closed either way.
  Result receive();

  // Drops the connection, whatever state it is in. Safe to call at any time,
  // and the last thing that happens to a request that failed.
  void close();

  // A request is open and taking body bytes: open() has succeeded, and neither
  // end() nor a failure has happened since.
  bool streaming() const { return _client != nullptr && !_ended; }

  // How the last call ended. Ok until something fails, and the answer's own
  // result once receive() has run.
  Result result() const { return _result; }

  // Meaningful after Ok. Always a valid string: empty before the first answer.
  const char* answer() const { return _answer; }

  // The HTTP status, once there was one. Zero when the request never got that
  // far, which is every NoServer and every TimedOut.
  int status() const { return _status; }

  // True when open() failed because nothing at all answered at the address --
  // a connect that ran out of its own budget rather than one that was refused.
  // It is the one distinction inside NoServer that the device can act on, and
  // the stale-lease rule in main.cpp is what acts on it: a refusal proves
  // something is at the address and therefore that the address works, while
  // silence is also the shape of a cached DHCP lease that has outlived its
  // network.
  bool unreachable() const { return _unreachable; }

  // What went up in this request, counted without the framing: the number the
  // backend's own byte count should match. Zero again on every open(), which
  // is what makes it the offset the next write() continues from.
  size_t sentBytes() const { return _sentBytes; }

  // The chunks that carried it, and the longest single write of one -- which is
  // where one of E7's stalls shows, if the request had one -- with the moment
  // that write started.
  uint32_t frames() const { return _frames; }
  uint32_t longestWriteUs() const { return _longestWriteUs; }
  int64_t longestWriteAtUs() const { return _longestWriteAtUs; }

  // What the reply weighed, for the log.
  size_t replyBytes() const { return _replyBytes; }

  // The request's own timeline, in esp_timer_get_time() microseconds, so it
  // shares an axis with the hold and the panel. Zero until each happens. After a
  // retry they are the retry's: the request that carried the question.
  //
  //   openUs       open() called
  //   connectedUs  connected, the headers sent
  //   endUs        the terminating chunk written -- the body is over
  //   firstByteUs  the response's headers in
  //   doneUs       the answer read, or the failure known
  //
  // firstByteUs is when this thread read the headers, and the orchestrator
  // comes to receive() only after the taken chirp: an answer that arrives inside
  // the chirp's 60 ms reads as the chirp, as the prototype backend's does. So
  // firstByteUs - endUs is the backend's own time only once the backend is
  // slower than a chirp.
  int64_t openUs() const { return _openUs; }
  int64_t connectedUs() const { return _connectedUs; }
  int64_t endUs() const { return _endUs; }
  int64_t firstByteUs() const { return _firstByteUs; }
  int64_t doneUs() const { return _doneUs; }

  // Why it ended that way, for Serial1. The screen gets one of four titles
  // instead; this is the string that says which of the ways it was.
  const char* lastError() const { return _error; }

 private:
  // The configured endpoint, written in one place so nothing else concatenates
  // it.
  void endpoint(char* out, size_t size) const;

  // One exit for every ending, the answer's and every failure's: stops the
  // clock, records how, and closes the connection. So a request that failed is
  // never left open, and receive() closes whichever way it went.
  Result finish(Result result, const char* format, ...);

  // Puts `bytes` on the socket, all of them or a failure. Times the write.
  bool send(const uint8_t* data, size_t bytes);

  const char* _baseUrl;
  const char* _token;

  esp_http_client* _client = nullptr;
  bool _ended = false;
  Result _result = Result::Ok;

  int64_t _openUs = 0;
  int64_t _connectedUs = 0;
  int64_t _endUs = 0;
  int64_t _firstByteUs = 0;
  int64_t _doneUs = 0;

  size_t _sentBytes = 0;
  uint32_t _frames = 0;
  uint32_t _longestWriteUs = 0;
  int64_t _longestWriteAtUs = 0;

  int _status = 0;
  size_t _replyBytes = 0;
  bool _unreachable = false;

  // The size line, the samples and the CRLF of one chunk.
  uint8_t _frame[kFrameBytes + 16];
  // The reply, and its terminator for the parser.
  char _reply[kMaxReplyBytes + 1];

  char _answer[kMaxAnswerChars] = {0};
  char _error[96] = {0};
};
