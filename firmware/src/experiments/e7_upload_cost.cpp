// E7 -- what the upload costs, and where the extra seconds in it come from.
// See docs/experiments.md. Build and flash with:
//
//     ~/.platformio/penv/bin/pio run -e exp_e7
//     ~/.platformio/penv/bin/pio run -e exp_e7 -t upload --upload-port <port>
//
// This replaces main.cpp in its own environment; nothing here is part of the
// firmware. S7 measured nine questions and got "between 260 ms and 7.4 s" out of
// them, with no relation to the payload: 400428 bytes went up in 2876 ms while
// 112684 took 6848 ms. The backend is not in it -- it swallows 120 KB in 4 ms
// over the LAN. So the seconds are somewhere between the socket and the air, and
// HTTPClient reports the whole round trip as one number, which is why S7 could
// not say where.
//
// This rig takes the same upload apart. Four things it separates that S7 could
// not:
//
//   the phases     a bare NetworkClient, so the connect, the header, the body
//                  and the wait for the status line are four clocks instead of
//                  one. The body goes out in 4 KB chunks, each timed, so a
//                  stall has an offset as well as a duration -- a TCP
//                  retransmission timeout is a single chunk that takes a whole
//                  second, and it looks nothing like a link that is merely slow.
//   power save     alternating wake by wake. E6 measured WiFi.setSleep(false)
//                  against the DHCP exchange and found nothing, but that is two
//                  round trips. An upload is hundreds of ACKs, which is where a
//                  station that sleeps between beacons would cost whole seconds.
//   the age of the link
//                  four uploads per wake, three seconds apart. Every question in
//                  S7 uploaded within a second or two of the address arriving,
//                  so a cost that belongs to a link that has just come up would
//                  have looked like a cost of the upload. Upload 1 is that
//                  question; uploads 2 and 3 are the same upload on a link that
//                  has settled.
//   the firmware's own path
//                  upload 4 goes through Backend, unchanged. It anchors the
//                  rig's four clocks to the one number S7 reported, so the
//                  finding transfers to the firmware rather than to the rig.
//                  Backend has been a chunked esp_http_client request since
//                  S11, where the result in docs/experiments.md went through
//                  HTTPClient; the payload goes up as a body of one write.
//
// **The connects that never arrive are the other half of the question.** Four of
// S7's questions could not open a connection to a backend that was running and
// gave up at the 5 s timeout, with nothing reaching the backend at all. That is
// about one in fifteen, and S7b plans to read exactly that as a stale DHCP
// lease -- so it has to be counted here. Every failed connect is retried once,
// immediately: a lost SYN and a path that is not there look the same from one
// attempt and different from two.
//
// The payload is Recording's own buffer, filled with a tone rather than by the
// microphone -- 4 s at 16 kHz mono, 128044 bytes with the WAV header, in the
// middle of the 34-400 KB S7 actually saw. It is the firmware's allocation, sent
// from PSRAM by pointer, so nothing about the body's shape is the rig's
// invention.
//
// The pacing is E1's: kAutoCycles wakes on the timer, unattended, and then the
// button, because the board has no off switch.

#include <Arduino.h>
#include <NetworkClient.h>
#include <WiFi.h>
#include <esp_timer.h>

#include "config.h"
#include "secrets.h"

#include "sticky/buzzer.h"
#include "sticky/power.h"

#include "backend.h"
#include "recording.h"
#include "wifi_link.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// Eight wakes of each power-save setting. Sixty-four uploads in all, which is
// the sample size the question needs: S7's failed connect happened about once
// in fifteen, and one or two of them would say nothing about how often it is.
constexpr uint32_t kAutoCycles = 16;
constexpr uint64_t kAutoSleepUs = 5000000;

constexpr uint32_t kUploadsPerWake = 4;

// Long enough that upload 2 is not still inside whatever upload 1 disturbed,
// short enough that the whole wake stays under twenty seconds.
constexpr uint32_t kGapMs = 3000;

// Four seconds of the firmware's own format. The header brings it to 128044.
constexpr uint32_t kPayloadSeconds = 4;

// The resolution of the body timeline. 4 KB is about three full-sized segments,
// so a chunk is short enough to put a stall within a few KB of where it happened
// and long enough that thirty-two of them fit on one line of the log.
constexpr size_t kChunkBytes = 4096;
constexpr size_t kMaxChunks = 64;

constexpr uint32_t kLogRows = kAutoCycles * kUploadsPerWake;
constexpr uint32_t kMagic = 0xE7C0FFEE;

enum Outcome : uint8_t {
  kOk = 0,
  kConnectFailed,  // nothing accepted a connection, twice
  kWriteFailed,    // the socket died with the body half sent
  kNoAnswer,       // the body went out and nothing came back
  kBadStatus,      // answered, and not with 200
};

const char* outcomeName(uint8_t outcome) {
  switch (outcome) {
    case kOk: return "ok";
    case kConnectFailed: return "no connect";
    case kWriteFailed: return "write died";
    case kNoAnswer: return "no answer";
    case kBadStatus: return "bad status";
    default: return "?";
  }
}

struct Row {
  uint8_t cycle;
  uint8_t index;   // 1..kUploadsPerWake
  bool sleepOff;   // this wake ran with WiFi.setSleep(false)
  bool viaHttp;    // upload 4: the firmware's own Backend
  uint16_t connectMs;
  uint16_t retryMs;  // a second connect, after the first gave up; 0 if not needed
  uint16_t headerMs;
  uint16_t bodyMs;
  uint16_t waitMs;  // the body written, to the first byte of the status line
  uint16_t readMs;
  uint16_t totalMs;
  uint16_t stallMs;    // the slowest single chunk of the body
  uint16_t stallAtKb;  // and where it started
  uint16_t status;
  int8_t rssi;
  uint8_t outcome;
};

RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_cycles;
RTC_DATA_ATTR uint32_t g_rows;
RTC_DATA_ATTR Row g_log[kLogRows];

WifiLink wifi;
Recording audio;
Backend backend;

uint16_t g_chunkMs[kMaxChunks];
uint32_t g_chunkCount = 0;

char g_host[48] = {0};
uint16_t g_port = 80;

constexpr const char* kRowFormat = "%3s %3s %-4s %2s %-6s %7s %6s %8s %7s %6s %8s %8s %10s %5s  %s\n";

// The tone that stands in for a recording. The content does not matter to a
// measurement of throughput -- what matters is that the payload is the size and
// shape of a real question, and that anyone who opens one of these on the
// backend hears something that is obviously the rig and not a recording.
void fillWithTone() {
  uint16_t phase = 0;
  constexpr uint16_t kStep = 1802;  // 440 Hz at 16 kHz, in 1/65536ths of a turn

  while (!audio.full()) {
    const uint32_t want = audio.nextChunkSamples();
    if (want == 0) break;
    int16_t* head = audio.writeHead();
    for (uint32_t i = 0; i < want; ++i) {
      const int32_t ramp = static_cast<int32_t>(phase) - 32768;
      const int32_t tri = ((ramp < 0 ? -ramp : ramp) * 2 - 32768) / 4;
      head[i] = static_cast<int16_t>(tri);
      phase = static_cast<uint16_t>(phase + kStep);
    }
    audio.commit(want);
  }
}

// One line of the response, with a deadline of its own. Stream::readStringUntil
// would do it, but its timeout is a different member from the one the socket
// uses and the two are easy to confuse -- and this rig is measuring time, so
// every wait in it should say how long it is willing to be.
int readLine(NetworkClient& client, char* out, size_t size, uint32_t budgetMs) {
  const int64_t start = esp_timer_get_time();
  size_t n = 0;
  for (;;) {
    const int c = client.read();
    if (c < 0) {
      if (client.available() == 0 && !client.connected()) break;
      if (static_cast<uint32_t>((esp_timer_get_time() - start) / 1000) >= budgetMs) return -1;
      delay(1);
      continue;
    }
    if (c == '\n') break;
    if (c != '\r' && n + 1 < size) out[n++] = static_cast<char>(c);
  }
  out[n] = '\0';
  return static_cast<int>(n);
}

// Everything the server still has to say, thrown away. The request asks for
// Connection: close, so the end of the body is the end of the socket.
size_t drainBody(NetworkClient& client, uint32_t budgetMs) {
  const int64_t start = esp_timer_get_time();
  size_t total = 0;
  uint8_t buf[256];
  for (;;) {
    const int avail = client.available();
    if (avail > 0) {
      const int got = client.read(buf, avail > static_cast<int>(sizeof(buf))
                                           ? sizeof(buf)
                                           : static_cast<size_t>(avail));
      if (got > 0) total += static_cast<size_t>(got);
      continue;
    }
    if (!client.connected()) break;
    if (static_cast<uint32_t>((esp_timer_get_time() - start) / 1000) >= budgetMs) break;
    delay(1);
  }
  return total;
}

// The upload, with the four clocks S7 could not tell apart.
//
// The connect goes to the host string rather than to an IPAddress, which is
// what HTTPClient does and therefore what the firmware pays. The host here is a
// dotted quad, so the resolution inside it is a parse and not a query -- but
// that is a property of config.h rather than of the code, and it is worth
// remembering before this number is read as "the TCP handshake".
Row postBare(const uint8_t* body, size_t bytes) {
  Row row = {};
  row.rssi = static_cast<int8_t>(WiFi.RSSI());
  g_chunkCount = 0;

  NetworkClient client;
  const int64_t tStart = esp_timer_get_time();

  bool open = client.connect(g_host, g_port, config::kBackendConnectTimeoutMs) == 1;
  row.connectMs = static_cast<uint16_t>((esp_timer_get_time() - tStart) / 1000);

  if (!open) {
    // The retry is the measurement, not a recovery: a SYN that was lost comes
    // back in milliseconds, and a path that is not there costs the timeout
    // twice. S7b reads this failure as a stale lease and needs to know which of
    // the two it is looking at.
    const int64_t tRetry = esp_timer_get_time();
    open = client.connect(g_host, g_port, config::kBackendConnectTimeoutMs) == 1;
    row.retryMs = static_cast<uint16_t>((esp_timer_get_time() - tRetry) / 1000);
    if (!open) {
      row.outcome = kConnectFailed;
      row.totalMs = static_cast<uint16_t>((esp_timer_get_time() - tStart) / 1000);
      return row;
    }
  }

  // Both are what HTTPClient sets on the socket it owns, so the bare path is
  // not a faster path -- it is the same path with more clocks in it.
  client.setNoDelay(true);
  client.setConnectionTimeout(config::kResponseTimeoutMs);

  char head[256];
  const int headLen =
      snprintf(head, sizeof(head),
               "POST %s HTTP/1.1\r\nHost: %s:%u\r\nContent-Type: audio/wav\r\n"
               "Content-Length: %u\r\nConnection: close\r\n\r\n",
               config::kAudioPath, g_host, static_cast<unsigned>(g_port),
               static_cast<unsigned>(bytes));

  const int64_t tHead = esp_timer_get_time();
  const size_t headSent = client.write(reinterpret_cast<const uint8_t*>(head),
                                       static_cast<size_t>(headLen));
  row.headerMs = static_cast<uint16_t>((esp_timer_get_time() - tHead) / 1000);
  if (headSent != static_cast<size_t>(headLen)) {
    row.outcome = kWriteFailed;
    row.totalMs = static_cast<uint16_t>((esp_timer_get_time() - tStart) / 1000);
    client.stop();
    return row;
  }

  const int64_t tBody = esp_timer_get_time();
  int64_t previous = tBody;
  size_t sent = 0;
  while (sent < bytes) {
    const size_t want = bytes - sent < kChunkBytes ? bytes - sent : kChunkBytes;
    const size_t wrote = client.write(body + sent, want);

    const int64_t now = esp_timer_get_time();
    const uint32_t chunkMs = static_cast<uint32_t>((now - previous) / 1000);
    previous = now;
    if (g_chunkCount < kMaxChunks) g_chunkMs[g_chunkCount++] = static_cast<uint16_t>(chunkMs);
    if (chunkMs > row.stallMs) {
      row.stallMs = static_cast<uint16_t>(chunkMs);
      row.stallAtKb = static_cast<uint16_t>(sent / 1024);
    }

    if (wrote != want) {
      // NetworkClient::write() only returns short when it has given up -- ten
      // one-second selects with nothing draining -- so this is the socket
      // dying, not backpressure.
      row.outcome = kWriteFailed;
      row.bodyMs = static_cast<uint16_t>((esp_timer_get_time() - tBody) / 1000);
      row.totalMs = static_cast<uint16_t>((esp_timer_get_time() - tStart) / 1000);
      client.stop();
      return row;
    }
    sent += wrote;
  }
  row.bodyMs = static_cast<uint16_t>((esp_timer_get_time() - tBody) / 1000);

  const int64_t tWait = esp_timer_get_time();
  char line[128];
  const int got = readLine(client, line, sizeof(line), config::kResponseTimeoutMs);
  row.waitMs = static_cast<uint16_t>((esp_timer_get_time() - tWait) / 1000);
  if (got <= 0) {
    row.outcome = kNoAnswer;
    row.totalMs = static_cast<uint16_t>((esp_timer_get_time() - tStart) / 1000);
    client.stop();
    return row;
  }

  unsigned status = 0;
  if (sscanf(line, "HTTP/%*d.%*d %u", &status) == 1) row.status = static_cast<uint16_t>(status);

  const int64_t tRead = esp_timer_get_time();
  while (readLine(client, line, sizeof(line), config::kResponseTimeoutMs) > 0) {
    // headers, to the blank line
  }
  drainBody(client, config::kResponseTimeoutMs);
  row.readMs = static_cast<uint16_t>((esp_timer_get_time() - tRead) / 1000);

  row.totalMs = static_cast<uint16_t>((esp_timer_get_time() - tStart) / 1000);
  row.outcome = row.status == 200 ? kOk : kBadStatus;
  client.stop();
  return row;
}

// Upload 4: the firmware's own path, so the four clocks above have something to
// be checked against. Backend reports two numbers rather than six, which
// is the whole reason this experiment exists.
Row postViaFirmware(const uint8_t* body, size_t bytes) {
  Row row = {};
  row.viaHttp = true;
  row.rssi = static_cast<int8_t>(WiFi.RSSI());
  g_chunkCount = 0;

  const bool sent = backend.open() && backend.write(body, bytes) && backend.end();
  const Backend::Result result = sent ? backend.receive() : backend.result();
  row.totalMs = static_cast<uint16_t>((backend.doneUs() - backend.openUs()) / 1000);
  if (backend.firstByteUs() != 0) {
    row.waitMs = static_cast<uint16_t>((backend.firstByteUs() - backend.openUs()) / 1000);
  }
  row.status = static_cast<uint16_t>(backend.status());

  switch (result) {
    case Backend::Result::Ok: row.outcome = kOk; break;
    case Backend::Result::NoServer: row.outcome = kConnectFailed; break;
    case Backend::Result::ServerError: row.outcome = kBadStatus; break;
    case Backend::Result::BadResponse: row.outcome = kBadStatus; break;
    case Backend::Result::TimedOut: row.outcome = kNoAnswer; break;
  }
  if (result != Backend::Result::Ok) {
    Serial1.printf("       Backend: %s\n", backend.lastError());
  }
  return row;
}

void printChunks(const Row& row) {
  if (g_chunkCount == 0) return;

  // Every chunk, every time. A stall is a shape rather than a maximum -- one
  // chunk at 1000 ms with the rest at 10 is a retransmission timeout, and
  // thirty-two chunks at 40 ms is a link that is simply slow -- and the shape
  // does not survive being reduced to a number.
  Serial1.printf("       %u KB in %u chunks, ms each:", static_cast<unsigned>(kChunkBytes / 1024),
                 static_cast<unsigned>(g_chunkCount));
  for (uint32_t i = 0; i < g_chunkCount; ++i) Serial1.printf(" %u", g_chunkMs[i]);
  Serial1.println();
  if (row.stallMs > 0) {
    Serial1.printf("       slowest chunk %u ms, at %u KB in\n", row.stallMs, row.stallAtKb);
  }
}

void describe(const Row& row, size_t bytes) {
  const uint32_t kbps = row.bodyMs > 0
                            ? static_cast<uint32_t>((bytes / 1024) * 1000 / row.bodyMs)
                            : 0;

  if (row.viaHttp) {
    Serial1.printf("  up %u  Backend: %lu ms to the first byte, %lu ms in all -> %u (%s)\n",
                   row.index, static_cast<unsigned long>(row.waitMs),
                   static_cast<unsigned long>(row.totalMs), row.status, outcomeName(row.outcome));
    return;
  }

  Serial1.printf("  up %u  connect %u ms", row.index, row.connectMs);
  if (row.retryMs > 0) Serial1.printf(" (failed, retry %u ms)", row.retryMs);
  Serial1.printf(", header %u ms, body %u ms (%lu KB/s), wait %u ms, read %u ms"
                 " -> %u, %u ms in all (%s)\n",
                 row.headerMs, row.bodyMs, static_cast<unsigned long>(kbps), row.waitMs,
                 row.readMs, row.status, row.totalMs, outcomeName(row.outcome));
}

void record(const Row& row) {
  if (g_rows < kLogRows) g_log[g_rows] = row;
  ++g_rows;
}

void printTable(size_t bytes) {
  Serial1.println();
  Serial1.printf(kRowFormat, "#", "cy", "ps", "up", "path", "connect", "retry", "header", "body",
                 "wait", "read", "total", "KB/s", "stall", "rssi  outcome");

  const uint32_t rows = g_rows < kLogRows ? g_rows : kLogRows;
  for (uint32_t i = 0; i < rows; ++i) {
    const Row& row = g_log[i];

    char index[8], cycle[8], up[8], connect[12], retry[12], header[12], body[12], wait[12],
        read[12], total[12], rate[12], stall[14], rssi[10];

    snprintf(index, sizeof(index), "%lu", static_cast<unsigned long>(i + 1));
    snprintf(cycle, sizeof(cycle), "%u", row.cycle);
    snprintf(up, sizeof(up), "%u", row.index);
    snprintf(connect, sizeof(connect), "%u ms", row.connectMs);
    snprintf(retry, sizeof(retry), row.retryMs > 0 ? "%u ms" : "--", row.retryMs);
    snprintf(header, sizeof(header), "%u ms", row.headerMs);
    snprintf(body, sizeof(body), "%u ms", row.bodyMs);
    snprintf(wait, sizeof(wait), "%u ms", row.waitMs);
    snprintf(read, sizeof(read), "%u ms", row.readMs);
    snprintf(total, sizeof(total), "%u ms", row.totalMs);
    snprintf(rssi, sizeof(rssi), "%d", row.rssi);

    if (row.bodyMs > 0) {
      snprintf(rate, sizeof(rate), "%lu",
               static_cast<unsigned long>((bytes / 1024) * 1000 / row.bodyMs));
    } else {
      snprintf(rate, sizeof(rate), "--");
    }
    if (row.stallMs > 0) {
      snprintf(stall, sizeof(stall), "%u@%uKB", row.stallMs, row.stallAtKb);
    } else {
      snprintf(stall, sizeof(stall), "--");
    }

    // The firmware's own path reports one number, and printing it under six
    // column headings would invent five.
    if (row.viaHttp) {
      snprintf(connect, sizeof(connect), "--");
      snprintf(header, sizeof(header), "--");
      snprintf(body, sizeof(body), "--");
      snprintf(read, sizeof(read), "--");
      snprintf(rate, sizeof(rate), "--");
    }

    char tail[32];
    snprintf(tail, sizeof(tail), "%4d  %s", row.rssi, outcomeName(row.outcome));

    Serial1.printf(kRowFormat, index, cycle, row.sleepOff ? "off" : "on", up,
                   row.viaHttp ? "http" : "bare", connect, retry, header, body, wait, read, total,
                   rate, stall, tail);
  }
  Serial1.println();
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  stickyPower::holdLatch();
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  if (g_magic != kMagic) {
    g_magic = kMagic;
    g_cycles = 0;
    g_rows = 0;
  }
  const uint32_t cycle = g_cycles + 1;

  // Power save off on even wakes, so both settings see the same network within
  // half a minute of each other rather than in two runs an hour apart.
  const bool sleepOff = (cycle % 2 == 0);

  Serial1.println();
  Serial1.printf("E7 upload rig -- cycle %lu, power save %s, wake %s\n",
                 static_cast<unsigned long>(cycle), sleepOff ? "OFF" : "on (the default)",
                 stickyPower::wakeupCauseName());

  if (sscanf(config::kBackendBaseUrl, "http://%47[^:/]:%hu", g_host, &g_port) < 1 ||
      g_host[0] == '\0') {
    Serial1.printf("FAILED: config::kBackendBaseUrl (\"%s\") is not http://host:port\n",
                   config::kBackendBaseUrl);
    Serial1.flush();
    stickyBuzzer::error();
    stickyPower::deepSleep();
  }

  if (!audio.begin(16000, kPayloadSeconds)) {
    Serial1.printf("FAILED: %s\n", audio.lastError());
    Serial1.flush();
    stickyBuzzer::error();
    stickyPower::deepSleep();
  }
  fillWithTone();
  const size_t bytes = audio.wavBytes();
  const uint8_t* body = audio.wav();
  Serial1.printf("  payload: %u bytes of WAV, %lu ms of tone, to http://%s:%u%s\n",
                 static_cast<unsigned>(bytes), static_cast<unsigned long>(audio.recordedMs()),
                 g_host, static_cast<unsigned>(g_port), config::kAudioPath);

  WiFi.persistent(false);
  WiFi.mode(WIFI_STA);

  // esp_wifi_set_ps() needs the station to exist, which WiFi.mode() is what
  // creates -- and deep sleep puts it back to the IDF's default on every wake,
  // so there is nothing to undo on the wakes that leave it alone.
  if (sleepOff) WiFi.setSleep(false);

  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword)) {
    Serial1.printf("FAILED: %s\n", wifi.lastError());
    Serial1.flush();
    stickyBuzzer::error();
    stickyPower::deepSleep();
  }
  while (wifi.poll() == WifiLink::State::Connecting) delay(2);

  if (!wifi.online()) {
    Serial1.printf("  no address after %lu ms: %s\n", static_cast<unsigned long>(wifi.elapsedMs()),
                   wifi.lastError());
    Serial1.flush();
    stickyBuzzer::error();
    wifi.end();
    stickyPower::deepSleep(g_cycles + 1 < kAutoCycles ? kAutoSleepUs : 0);
  }

  Serial1.printf("  online in %lu ms (link %lu ms), %s, ch %u, %d dBm, power save %s\n",
                 static_cast<unsigned long>(wifi.elapsedMs()),
                 static_cast<unsigned long>(wifi.linkMs()), wifi.ip(), wifi.channel(), wifi.rssi(),
                 WiFi.getSleep() ? "on" : "off");
  Serial1.printf("  top of setup() to a usable network: %lu ms\n",
                 static_cast<unsigned long>((esp_timer_get_time() - tEntry) / 1000));

  for (uint32_t i = 1; i <= kUploadsPerWake; ++i) {
    if (i > 1) delay(kGapMs);

    const bool viaFirmware = (i == kUploadsPerWake);
    Row row = viaFirmware ? postViaFirmware(body, bytes) : postBare(body, bytes);
    row.cycle = static_cast<uint8_t>(cycle);
    row.index = static_cast<uint8_t>(i);
    row.sleepOff = sleepOff;

    describe(row, bytes);
    if (!viaFirmware) printChunks(row);
    record(row);
  }

  ++g_cycles;
  printTable(bytes);

  wifi.end();
  audio.end();

  const bool autoCycle = g_cycles < kAutoCycles;
  Serial1.println(autoCycle ? "  sleeping" : "  press the AI button for another cycle");
  Serial1.flush();
  stickyBuzzer::ready();
  stickyPower::deepSleep(autoCycle ? kAutoSleepUs : 0);
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
