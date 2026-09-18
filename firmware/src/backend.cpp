#include "backend.h"

#include <ArduinoJson.h>
#include <esp_http_client.h>
#include <esp_timer.h>
#include <esp_tls_errors.h>
#include <lwip/sockets.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "config.h"

namespace {

// The terminating chunk: a chunk of length zero, and the empty trailer.
constexpr char kLastChunk[] = "0\r\n\r\n";

uint32_t millisSince(int64_t fromUs) {
  return static_cast<uint32_t>((esp_timer_get_time() - fromUs) / 1000);
}

}  // namespace

Backend::~Backend() { close(); }

void Backend::endpoint(char* out, size_t size) {
  snprintf(out, size, "%s%s", config::kBackendBaseUrl, config::kAudioPath);
}

bool Backend::open() {
  char url[kMaxUrlChars];
  endpoint(url, sizeof(url));
  return open(url);
}

bool Backend::open(const char* url) {
  close();

  _ended = false;
  _result = Result::Ok;
  _openUs = esp_timer_get_time();
  _connectedUs = 0;
  _endUs = 0;
  _firstByteUs = 0;
  _doneUs = 0;
  _sentBytes = 0;
  _frames = 0;
  _longestWriteUs = 0;
  _longestWriteAtUs = 0;
  _status = 0;
  _replyBytes = 0;
  _unreachable = false;
  _answer[0] = '\0';
  _error[0] = '\0';

  // The connect's budget is the only thing timeout_ms is here -- see
  // kBackendConnectTimeoutMs in config.h, and below for the two waits that
  // follow it. esp-tls also puts it on the socket as SO_SNDTIMEO, which only
  // splits a send that blocks longer than that into two; the write's own poll
  // is what decides the connection has failed.
  esp_http_client_config_t cfg = {};
  cfg.url = url;
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = static_cast<int>(config::kBackendConnectTimeoutMs);
  cfg.disable_auto_redirect = true;

  _client = esp_http_client_init(&cfg);
  if (_client == nullptr) {
    finish(Result::NoServer, "esp_http_client would not take \"%s\"", url);
    return false;
  }

  esp_http_client_set_header(_client, "Content-Type", "audio/wav");

  // No keep-alive: the answer is the last thing that happens before the chip
  // goes away, so a connection held open is one the backend has to time out by
  // itself.
  esp_http_client_set_header(_client, "Connection", "close");

  // A negative length is what makes the body chunked: the library writes
  // Transfer-Encoding instead of Content-Length, and nothing else.
  const esp_err_t opened = esp_http_client_open(_client, -1);
  if (opened != ESP_OK) {
    // esp-tls keeps its own verdict on the connect apart from the socket's
    // errno, and the verdict is what separates a port that said no from an
    // address that said nothing: a select() that ran out of the budget is
    // ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT, and a refusal is a failed connect
    // with the socket's errno behind it -- 104, ECONNRESET, which is what lwIP
    // makes of the RST a closed port answers with. The first is the shape a
    // stale lease has.
    const int socketErrno = esp_http_client_get_errno(_client);
    const esp_err_t verdict =
        esp_http_client_get_and_clear_last_tls_error(_client, nullptr, nullptr);
    if (verdict == ESP_ERR_ESP_TLS_CONNECTION_TIMEOUT) {
      _unreachable = true;
      finish(Result::NoServer, "nothing answered at the address in %lu ms",
             static_cast<unsigned long>(config::kBackendConnectTimeoutMs));
      return false;
    }
    finish(Result::NoServer, "%s after %lu ms (%s, errno %d)", esp_err_to_name(opened),
           static_cast<unsigned long>(millisSince(_openUs)), esp_err_to_name(verdict),
           socketErrno);
    return false;
  }

  // From here the connection has answered, so the wait is for a stall rather
  // than for an address -- see kBackendStallTimeoutMs.
  esp_http_client_set_timeout_ms(_client, static_cast<int>(config::kBackendStallTimeoutMs));

  // Nagle's algorithm holds a small segment back until everything before it has
  // been acknowledged, and a backend that delays its ACKs would then sit on the
  // terminating chunk -- the one write whose latency is the whole wait. Each
  // chunk is framed into a single write for the same reason, so nothing is lost
  // by switching it off. Failing to is not worth failing the question for.
  const int fd = esp_http_client_get_socket(_client);
  const int noDelay = 1;
  if (fd >= 0) setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

  _connectedUs = esp_timer_get_time();
  return true;
}

bool Backend::write(const uint8_t* data, size_t bytes) {
  if (!streaming()) {
    if (_result == Result::Ok) finish(Result::NoServer, "write() with no request open");
    return false;
  }

  while (bytes > 0) {
    const size_t samples = bytes < kFrameBytes ? bytes : kFrameBytes;

    // <size in hex>\r\n<samples>\r\n, in one buffer and one write.
    const int head = snprintf(reinterpret_cast<char*>(_frame), sizeof(_frame) - kFrameBytes,
                              "%x\r\n", static_cast<unsigned>(samples));
    memcpy(_frame + head, data, samples);
    _frame[head + samples] = '\r';
    _frame[head + samples + 1] = '\n';

    if (!send(_frame, head + samples + 2)) return false;

    _sentBytes += samples;
    ++_frames;
    data += samples;
    bytes -= samples;
  }
  return true;
}

bool Backend::end() {
  if (!streaming()) {
    if (_result == Result::Ok) finish(Result::NoServer, "end() with no request open");
    return false;
  }
  if (!send(reinterpret_cast<const uint8_t*>(kLastChunk), sizeof(kLastChunk) - 1)) return false;

  _ended = true;
  _endUs = esp_timer_get_time();
  return true;
}

bool Backend::send(const uint8_t* data, size_t bytes) {
  const int64_t startUs = esp_timer_get_time();
  const int written =
      esp_http_client_write(_client, reinterpret_cast<const char*>(data), static_cast<int>(bytes));
  const uint32_t tookUs = static_cast<uint32_t>(esp_timer_get_time() - startUs);

  if (tookUs > _longestWriteUs) {
    _longestWriteUs = tookUs;
    _longestWriteAtUs = startUs;
  }
  if (written == static_cast<int>(bytes)) return true;

  // Anything short is a failure, whatever went out before it: the library
  // returns 0 for a poll that ran out of the stall budget even when part of the
  // chunk had already gone, so the count is not worth trusting and the
  // connection is not worth keeping. A retry starts from byte zero anyway.
  if (written == 0) {
    finish(Result::NoServer, "the connection took nothing for %lu ms, %u bytes in",
           static_cast<unsigned long>(config::kBackendStallTimeoutMs),
           static_cast<unsigned>(_sentBytes));
  } else {
    finish(Result::NoServer, "the connection failed %u bytes in (errno %d)",
           static_cast<unsigned>(_sentBytes), esp_http_client_get_errno(_client));
  }
  return false;
}

Backend::Result Backend::receive() {
  if (_client == nullptr || !_ended) {
    if (_result != Result::Ok) return _result;
    return finish(Result::NoServer, "receive() with no finished request");
  }

  // The budget counts from the terminating chunk, not from this call: the taken
  // chirp stands between the two, and it is the backend's time that is being
  // bounded. The library applies it to each read, which for an answer that
  // arrives in one piece is the same thing.
  const uint32_t sinceEndMs = millisSince(_endUs);
  const uint32_t budgetMs =
      sinceEndMs < config::kResponseTimeoutMs ? config::kResponseTimeoutMs - sinceEndMs : 1;
  esp_http_client_set_timeout_ms(_client, static_cast<int>(budgetMs));

  const int64_t declared = esp_http_client_fetch_headers(_client);
  _firstByteUs = esp_timer_get_time();

  if (declared == -ESP_ERR_HTTP_EAGAIN) {
    return finish(Result::TimedOut, "nothing came back in %lu ms",
                  static_cast<unsigned long>(config::kResponseTimeoutMs));
  }
  if (declared < 0) {
    return finish(Result::NoServer, "the connection closed before the answer (errno %d)",
                  esp_http_client_get_errno(_client));
  }

  _status = esp_http_client_get_status_code(_client);
  if (_status != 200) return finish(Result::ServerError, "HTTP %d", _status);

  if (declared > static_cast<int64_t>(kMaxReplyBytes)) {
    return finish(Result::BadResponse, "a %lld-byte reply is not an answer",
                  static_cast<long long>(declared));
  }

  size_t got = 0;
  while (got < kMaxReplyBytes) {
    const int n =
        esp_http_client_read(_client, _reply + got, static_cast<int>(kMaxReplyBytes - got));
    if (n == -ESP_ERR_HTTP_EAGAIN) {
      _replyBytes = got;
      return finish(Result::TimedOut, "the reply stopped after %u bytes",
                    static_cast<unsigned>(got));
    }
    if (n < 0) {
      _replyBytes = got;
      return finish(Result::NoServer, "the connection failed %u bytes into the reply",
                    static_cast<unsigned>(got));
    }
    if (n == 0) break;
    got += static_cast<size_t>(n);
  }
  _replyBytes = got;

  // A body that stopped short is the answer running out of time halfway, not a
  // backend sending nonsense, and parsing what did arrive would call it the
  // second. The library knows whether it has all of it, declared length or not.
  if (!esp_http_client_is_complete_data_received(_client)) {
    if (got == kMaxReplyBytes) {
      return finish(Result::BadResponse, "a reply of more than %u bytes is not an answer",
                    static_cast<unsigned>(kMaxReplyBytes));
    }
    return finish(Result::TimedOut, "the reply stopped after %u of %lld bytes",
                  static_cast<unsigned>(got), static_cast<long long>(declared));
  }
  _reply[got] = '\0';

  // Hand-rolled extraction of one field breaks on the first escape sequence,
  // and an answer is exactly the kind of string that carries quotes. ArduinoJson
  // also decodes \uXXXX into UTF-8 by itself, which is what makes D1 a
  // backend-side decision rather than a parsing one.
  JsonDocument doc;
  const DeserializationError parsed = deserializeJson(doc, _reply, got);
  if (parsed) return finish(Result::BadResponse, "%s", parsed.c_str());

  // Null when the field is missing and null when it is there but is not a
  // string, which the vision's table counts as the same failure.
  const char* text = doc["response"].as<const char*>();
  if (text == nullptr) return finish(Result::BadResponse, "no \"response\" string in the reply");

  snprintf(_answer, sizeof(_answer), "%s", text);
  return finish(Result::Ok, "");
}

void Backend::close() {
  if (_client == nullptr) return;
  esp_http_client_cleanup(_client);
  _client = nullptr;
}

Backend::Result Backend::finish(Result result, const char* format, ...) {
  _doneUs = esp_timer_get_time();
  _result = result;

  va_list args;
  va_start(args, format);
  vsnprintf(_error, sizeof(_error), format, args);
  va_end(args);

  close();
  return result;
}
