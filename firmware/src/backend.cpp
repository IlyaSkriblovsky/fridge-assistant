#include "backend.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClient.h>
#include <esp_timer.h>
#include <stdarg.h>
#include <stdio.h>

#include "config.h"

// HTTPClient takes its read timeout as a uint16_t. A config.h that ever asked
// for more than 65.5 s would wrap to something short and the device would
// report TIMED OUT while the backend was still thinking -- which is a failure
// that looks like the network rather than like an edit. Caught at the build
// instead.
static_assert(config::kResponseTimeoutMs <= UINT16_MAX,
              "config::kResponseTimeoutMs does not fit HTTPClient::setTimeout()");

void Backend::endpoint(char* out, size_t size) {
  snprintf(out, size, "%s%s", config::kBackendBaseUrl, config::kAudioPath);
}

Backend::Result Backend::ask(const uint8_t* wav, size_t bytes) {
  char url[kMaxUrlChars];
  endpoint(url, sizeof(url));
  return ask(url, wav, bytes);
}

Backend::Result Backend::ask(const char* url, const uint8_t* wav, size_t bytes) {
  _startUs = esp_timer_get_time();
  _elapsedMs = 0;
  _firstByteMs = 0;
  _status = 0;
  _bodyBytes = 0;
  _unreachable = false;
  _answer[0] = '\0';
  _error[0] = '\0';

  // Both on the stack. There is one question per wake and deep sleep at the end
  // of it, so nothing is kept between two of them and nothing has to be reset
  // before one.
  NetworkClient client;
  HTTPClient http;

  if (!http.begin(client, url)) {
    return finish(Result::NoServer, "HTTPClient would not take \"%s\"", url);
  }

  // No keep-alive: the answer is the last thing that happens before the chip
  // goes away, so a connection held open is one the backend has to time out by
  // itself.
  http.setReuse(false);

  // The two waits are separate on purpose. A backend that is thinking is normal
  // and gets kResponseTimeoutMs; a backend that will not accept a connection is
  // not, and the shorter wait is what keeps a wrong address from costing the
  // whole budget -- see kBackendConnectTimeoutMs in config.h.
  http.setConnectTimeout(config::kBackendConnectTimeoutMs);
  http.setTimeout(config::kResponseTimeoutMs);
  http.addHeader("Content-Type", "audio/wav");

  // POST() takes a non-const pointer although it only ever reads through it.
  // The cast is what keeps the body a pointer into PSRAM rather than a copy of
  // it -- see the header.
  const int code = http.POST(const_cast<uint8_t*>(wav), bytes);
  _firstByteMs = static_cast<uint32_t>((esp_timer_get_time() - _startUs) / 1000);

  if (code == HTTPC_ERROR_READ_TIMEOUT) {
    return finish(Result::TimedOut, "nothing came back in %lu ms",
                  static_cast<unsigned long>(config::kResponseTimeoutMs));
  }

  // Everything else negative is the connection: refused, never established,
  // lost while the recording was going up. One screen covers all of it, and
  // this string is where the difference survives.
  //
  // HTTPClient calls every failure to connect "connection refused", so the
  // clock is the only thing that separates a port that said no from an address
  // that said nothing at all. Worth separating: the second is the shape a stale
  // cached lease will have from S7b, and it is the one that costs five seconds.
  if (code == HTTPC_ERROR_CONNECTION_REFUSED &&
      _firstByteMs >= config::kBackendConnectTimeoutMs) {
    _unreachable = true;
    return finish(Result::NoServer, "nothing answered at the address in %lu ms",
                  static_cast<unsigned long>(config::kBackendConnectTimeoutMs));
  }
  if (code < 0) {
    return finish(Result::NoServer, "%s (%d)", HTTPClient::errorToString(code).c_str(), code);
  }

  _status = code;
  if (code != HTTP_CODE_OK) return finish(Result::ServerError, "HTTP %d", code);

  const int declared = http.getSize();
  if (declared > static_cast<int>(kMaxBodyBytes)) {
    return finish(Result::BadResponse, "a %d-byte reply is not an answer", declared);
  }

  const String body = http.getString();
  _bodyBytes = body.length();

  // A body that stopped short is the answer running out of time halfway, not a
  // backend sending nonsense. getString() reports the difference by not
  // reporting it -- it drops the read error and returns what it has -- so the
  // declared length is the only thing left that can tell the two apart.
  if (declared > 0 && _bodyBytes < static_cast<size_t>(declared)) {
    return finish(Result::TimedOut, "the reply stopped after %u of %d bytes",
                  static_cast<unsigned>(_bodyBytes), declared);
  }

  // Hand-rolled extraction of one field breaks on the first escape sequence,
  // and an answer is exactly the kind of string that carries quotes. ArduinoJson
  // also decodes \uXXXX into UTF-8 by itself, which is what makes D1 a
  // backend-side decision rather than a parsing one.
  JsonDocument doc;
  const DeserializationError parsed = deserializeJson(doc, body);
  if (parsed) return finish(Result::BadResponse, "%s", parsed.c_str());

  // Null when the field is missing and null when it is there but is not a
  // string, which the vision's table counts as the same failure.
  const char* text = doc["response"].as<const char*>();
  if (text == nullptr) return finish(Result::BadResponse, "no \"response\" string in the reply");

  snprintf(_answer, sizeof(_answer), "%s", text);
  return finish(Result::Ok, "");
}

Backend::Result Backend::finish(Result result, const char* format, ...) {
  _elapsedMs = static_cast<uint32_t>((esp_timer_get_time() - _startUs) / 1000);

  va_list args;
  va_start(args, format);
  vsnprintf(_error, sizeof(_error), format, args);
  va_end(args);

  return result;
}
