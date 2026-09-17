// S7 -- Upload and answer. A temporary driver, the way S1's to S6's were:
// docs/implementation.md asks for the byte count on the screen matching the
// recording's length, and for each row of the vision's error table provoked
// deliberately. S8 replaces all of this with the orchestrator.
//
// So this is the whole chain for the first time -- record, associate, POST,
// draw -- with one thing added that the orchestrator will not have: the target
// changes on every question. Each press walks one row of the table and the next
// press walks the next, which is what makes the error paths testable without a
// rebuild between them:
//
//   1  the real endpoint             the answer, and the byte count in it
//   2  a closed port                 NO SERVER, refused at once
//   3  an address nothing answers    NO SERVER, at the connect timeout
//   4  /audio/fault/500              SERVER ERROR 500
//   5  /audio/fault/empty            BAD RESPONSE
//   6  /audio/fault/slow             TIMED OUT, at kResponseTimeoutMs
//
// The three fault paths are endpoints of the prototype backend that exist for
// exactly this -- see its README. Rows 2 and 3 need no backend at all.
//
// **After the last row every press is row 1 again**, so the table is walked
// once and the happy path is what the device does from then on. The counter
// lives in RTC memory, which the reset that flashing or opening the monitor
// causes clears -- so a session always starts at row 1 and a walk of the table
// is six presses from a fresh flash.
//
// A press too short to be a question is discarded without consuming a row, and
// so is a question that never reached the POST: a row counts as walked when the
// round trip it was testing actually happened.

#include <Arduino.h>
#include <esp_timer.h>
#include <string.h>

#include "config.h"
#include "secrets.h"
#include "sticky_audio.h"
#include "sticky_backend.h"
#include "sticky_button.h"
#include "sticky_buzzer.h"
#include "sticky_capture.h"
#include "sticky_mic.h"
#include "sticky_power.h"
#include "sticky_screen.h"
#include "sticky_wifi.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// How often the association and the capture task are looked at while the button
// is held. Small, because it is also the resolution of StickyWifi::linkMs().
constexpr uint32_t kPollMs = 10;

// A press held through the report would wake the board again the moment it goes
// to sleep, since ext1 wakes on the level rather than on an edge.
constexpr uint32_t kReleaseWaitMs = 30000;

// What a question came to, which is the last column of the table below. The
// first five are StickyBackend's results; the rest are the ways a question ends
// before there is anything to send.
enum class Outcome : uint8_t {
  Answered,
  NoServer,
  ServerError,
  BadResponse,
  TimedOut,
  NoWifi,
  Broken,  // the microphone, the buffer or the panel
  Tap,     // too short to be a question
};

struct Target {
  const char* what;      // what this press is testing
  const char* expected;  // what the vision's table says should come of it
  const char* path;      // appended to config::kBackendBaseUrl...
  uint16_t port;         // ...with this port instead, when it is not 0
  const char* url;       // or, when set, the whole URL and the two above unused
};

// TEST-NET-1 (RFC 5737) is reserved for documentation and is routed nowhere, so
// row 3 is a connect that gets no answer at all rather than a refusal -- which
// is the shape S7b's stale lease will have.
constexpr Target kTargets[] = {
    {"the real endpoint", "the answer, with the recording's byte count in it",
     config::kAudioPath, 0, nullptr},
    {"a closed port", "NO SERVER, refused at once", config::kAudioPath, 1, nullptr},
    {"an address nothing answers at", "NO SERVER, at the connect timeout", nullptr, 0,
     "http://192.0.2.1/audio"},
    {"a backend that fails", "SERVER ERROR 500", "/audio/fault/500", 0, nullptr},
    {"a reply with no answer in it", "BAD RESPONSE", "/audio/fault/empty", 0, nullptr},
    {"a backend that never answers", "TIMED OUT, at the response timeout",
     "/audio/fault/slow", 0, nullptr},
};
constexpr uint32_t kTargetCount = sizeof(kTargets) / sizeof(kTargets[0]);

// The log lives in RTC memory, which survives deep sleep but not a power cycle
// or a reset -- so a session is exactly what this table holds.
constexpr uint32_t kLogRows = 16;
constexpr uint32_t kMagic = 0x57C0FFEE;

struct Row {
  uint8_t target;
  uint8_t outcome;
  uint16_t heldMs;
  uint16_t recordedMs;
  uint16_t waitedMs;  // release to a usable network, 0 when it was already up
  uint16_t answeredMs;  // release to the answer being on the glass
  uint32_t sentBytes;
  uint16_t roundTripMs;
  int16_t status;
};

RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_walked;     // rows of the table walked so far
RTC_DATA_ATTR uint32_t g_questions;  // rows written, which may exceed kLogRows
RTC_DATA_ATTR Row g_log[kLogRows];

StickyButton button;
StickyMic mic;
StickyAudio audio;
StickyCapture capture;
StickyWifi wifi;
StickyScreen screen;
StickyBackend backend;

// Everything a question produced, filled in as it goes and written to the table
// whichever way it ends.
Row g_row;

// The two moments the question is measured against, and neither is the moment
// the code reaches them.
//
//  * The release is tEntry plus the hold, because the orchestrator can be
//    inside a two and a half second panel refresh when the button comes up --
//    the capture task sees it, this thread does not. Measuring from where the
//    code notices would credit the refresh's own time to the network.
//  * Online is the first poll that reports it, which is at kPollMs.
//
// Both are zero until they happen.
int64_t g_releaseUs = 0;
int64_t g_onlineUs = 0;

// The row this question walks: the next one while the table is unfinished, and
// the real endpoint once it has been walked through.
uint32_t targetIndex() { return g_walked < kTargetCount ? g_walked : 0; }

const char* outcomeName(uint8_t outcome) {
  switch (static_cast<Outcome>(outcome)) {
    case Outcome::Answered: return "answered";
    case Outcome::NoServer: return "NO SERVER";
    case Outcome::ServerError: return "SERVER ERROR";
    case Outcome::BadResponse: return "BAD RESPONSE";
    case Outcome::TimedOut: return "TIMED OUT";
    case Outcome::NoWifi: return "NO WIFI";
    case Outcome::Broken: return "broken";
    case Outcome::Tap: return "tap, discarded";
    default: return "?";
  }
}

// The configured base URL with its port replaced. kBackendBaseUrl is
// "scheme://host[:port]", so the colon to cut at is the last one after the "//"
// -- the only other colon in the string belongs to the scheme.
void urlWithPort(char* out, size_t size, uint16_t port, const char* path) {
  const char* base = config::kBackendBaseUrl;
  const char* authority = strstr(base, "//");
  const char* colon = authority != nullptr ? strrchr(authority, ':') : nullptr;
  const size_t keep = colon != nullptr ? static_cast<size_t>(colon - base) : strlen(base);
  snprintf(out, size, "%.*s:%u%s", static_cast<int>(keep), base, static_cast<unsigned>(port),
           path);
}

void targetUrl(char* out, size_t size, const Target& target) {
  if (target.url != nullptr) {
    snprintf(out, size, "%s", target.url);
  } else if (target.port != 0) {
    urlWithPort(out, size, target.port, target.path);
  } else {
    snprintf(out, size, "%s%s", config::kBackendBaseUrl, target.path);
  }
}

void printTable() {
  static constexpr const char* kFormat = "%3s  %-30s %8s %8s %9s %11s %10s  %s\n";

  Serial1.println();
  Serial1.printf(kFormat, "#", "target", "held", "wait", "wav", "round trip", "to glass",
                 "outcome");

  const uint32_t rows = g_questions < kLogRows ? g_questions : kLogRows;
  for (uint32_t i = 0; i < rows; ++i) {
    const Row& row = g_log[i];

    char index[8];
    char held[12] = "--";
    char wait[12] = "--";
    char sent[12] = "--";
    char roundTrip[12] = "--";
    char answered[12] = "--";
    char outcome[32];

    snprintf(index, sizeof(index), "%lu", static_cast<unsigned long>(i + 1));
    if (row.heldMs != 0) snprintf(held, sizeof(held), "%u ms", row.heldMs);
    if (row.heldMs != 0) snprintf(wait, sizeof(wait), "%u ms", row.waitedMs);
    if (row.sentBytes != 0) snprintf(sent, sizeof(sent), "%lu B",
                                     static_cast<unsigned long>(row.sentBytes));
    if (row.roundTripMs != 0) snprintf(roundTrip, sizeof(roundTrip), "%u ms", row.roundTripMs);
    if (row.answeredMs != 0) snprintf(answered, sizeof(answered), "%u ms", row.answeredMs);

    if (static_cast<Outcome>(row.outcome) == Outcome::ServerError) {
      snprintf(outcome, sizeof(outcome), "SERVER ERROR %d", static_cast<int>(row.status));
    } else {
      snprintf(outcome, sizeof(outcome), "%s", outcomeName(row.outcome));
    }

    Serial1.printf(kFormat, index, kTargets[row.target].what, held, wait, sent, roundTrip,
                   answered, outcome);
  }
  Serial1.println();
}

// A press held through the report is still down when the sleep is armed, and
// ext1 wakes on the level. Only the 30 s cap can get here with the button down.
void waitForRelease() {
  if (!button.isDown()) return;

  const int64_t deadlineUs = esp_timer_get_time() + kReleaseWaitMs * 1000LL;
  Serial1.println("  waiting for the button to come back up");
  while (button.isDown() && esp_timer_get_time() < deadlineUs) delay(5);
}

// The one exit. `consumed` says whether the target this question was walking
// has been walked -- a tap or a failure before the POST leaves it for the next
// press, because the row it was testing never ran.
[[noreturn]] void finish(Outcome outcome, bool consumed) {
  g_row.outcome = static_cast<uint8_t>(outcome);

  // Called straight after whichever screen was drawn, so this is the whole gap
  // the user sits through: the network, the round trip and the refresh.
  if (g_releaseUs != 0) {
    g_row.answeredMs = static_cast<uint16_t>((esp_timer_get_time() - g_releaseUs) / 1000);
  }

  if (g_magic != kMagic) {
    g_magic = kMagic;
    g_questions = 0;
  }
  if (g_questions < kLogRows) g_log[g_questions] = g_row;
  ++g_questions;

  if (consumed) ++g_walked;

  printTable();

  mic.end();
  wifi.end();
  waitForRelease();

  Serial1.printf("  press the AI button for \"%s\"\n", kTargets[targetIndex()].what);
  Serial1.flush();

  stickyPower::deepSleep();
}

// A failure with nothing sent: log it, show it, chirp, sleep. The target stays
// where it is.
[[noreturn]] void fail(Outcome outcome, const char* title, const char* detail) {
  Serial1.printf("  %s: %s\n", title, detail != nullptr ? detail : "");
  stickyBuzzer::error();
  if (screen.begin()) screen.error(title, detail);
  finish(outcome, false);
}

}  // namespace

void setup() {
  const int64_t tEntry = esp_timer_get_time();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(tEntry);  // takes GPIO4 back from the RTC pad and pulls it up

  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  delay(50);

  if (g_magic != kMagic) {
    g_magic = kMagic;
    g_questions = 0;
    g_walked = 0;
  }

  const uint32_t index = targetIndex();
  const Target& target = kTargets[index];
  g_row = Row{};
  g_row.target = static_cast<uint8_t>(index);

  char url[StickyBackend::kMaxUrlChars];
  targetUrl(url, sizeof(url), target);

  Serial1.println();
  Serial1.printf("S7 upload driver -- question %lu, wake %s, reset %s\n",
                 static_cast<unsigned long>(g_questions + 1), stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());
  Serial1.printf("  target %lu/%lu: %s -- %s\n", static_cast<unsigned long>(index + 1),
                 static_cast<unsigned long>(kTargetCount), target.what, url);
  Serial1.printf("  expecting %s\n", target.expected);

  // The microphone comes first and the chirp comes after it: the chirp means
  // "the microphone is live", so nothing that can delay capture may sit between
  // them. E1 and E3, and the vision's step 3.
  if (!mic.begin()) fail(Outcome::Broken, "NO MICROPHONE", mic.lastError());
  if (!audio.begin(mic.sampleRate())) fail(Outcome::Broken, "NO MEMORY", audio.lastError());
  if (!capture.start(mic, audio, button)) {
    fail(Outcome::Broken, "NO MICROPHONE", "the capture task would not start");
  }
  stickyBuzzer::ready();

  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword)) {
    capture.abort();
    capture.wait();
    fail(Outcome::NoWifi, "NO WIFI", wifi.lastError());
  }

  // The one failure with nothing to draw a message on: chirp, log and sleep.
  if (!screen.begin()) {
    Serial1.printf("  the panel would not start: %s\n", screen.lastError());
    stickyBuzzer::error();
    capture.abort();
    capture.wait();
    finish(Outcome::Broken, false);
  }

  // A cold start is the only time the controller's previous-image RAM has
  // nothing to do with what is on the glass. On a wake it is one to two seconds
  // spent flushing a panel the next screen overwrites anyway -- see S8.
  if (!stickyPower::wokeFromDeepSleep()) screen.clear();
  screen.listening();

  // Everything from here until the release is the capture task's; this loop
  // only watches. A drop is the vision's rule that a recording with nowhere to
  // go is aborted rather than finished.
  while (!capture.finished()) {
    const StickyWifi::State state = wifi.poll();
    if (state == StickyWifi::State::Failed) capture.abort();
    if (state == StickyWifi::State::Online && g_onlineUs == 0) g_onlineUs = esp_timer_get_time();
    delay(kPollMs);
  }

  mic.end();

  g_row.heldMs = static_cast<uint16_t>(button.heldMs());
  g_row.recordedMs = static_cast<uint16_t>(audio.recordedMs());
  g_releaseUs = tEntry + static_cast<int64_t>(button.heldMs()) * 1000;

  Serial1.printf("  recording: %s after %lu ms held -- %lu ms of audio, %lu bytes to send\n",
                 capture.stopName(), static_cast<unsigned long>(button.heldMs()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(audio.wavBytes()));

  if (capture.stop() == StickyCapture::Stop::ReadFailed) {
    fail(Outcome::Broken, "NO MICROPHONE", capture.lastError());
  }
  if (button.isTap()) {
    Serial1.println("  too short to be a question -- nothing sent, target unchanged");
    finish(Outcome::Tap, false);
  }
  // **A release that arrives before the address does is not a failure.** The
  // association has a budget of its own -- config::kWifiConnectTimeoutMs -- and
  // NO WIFI is what happens when that runs out, not what happens when the
  // question was shorter than a DHCP exchange. S6 measured the gap and left it
  // here: a one second hold reaches the release with roughly 2.4 s of connect
  // still to go, and the recording waits for it rather than being thrown away.
  //
  // The vision's abort rule is about the other case, and the loop above is
  // where it lives: an association that *failed* while the button was down ends
  // the recording immediately rather than letting the user finish talking into
  // something with nowhere to go.
  while (wifi.poll() == StickyWifi::State::Connecting) delay(kPollMs);
  if (g_onlineUs == 0 && wifi.online()) g_onlineUs = esp_timer_get_time();
  if (g_onlineUs > g_releaseUs) {
    g_row.waitedMs = static_cast<uint16_t>((g_onlineUs - g_releaseUs) / 1000);
  }

  if (!wifi.online()) {
    Serial1.printf("  no network after %lu ms: %s\n", static_cast<unsigned long>(wifi.elapsedMs()),
                   wifi.lastError());
    fail(Outcome::NoWifi, "NO WIFI", nullptr);
  }

  Serial1.printf("  online in %lu ms (%s), %s -- %lu ms of it waited for after the release\n",
                 static_cast<unsigned long>(wifi.elapsedMs()),
                 wifi.usedCache() ? "cached AP" : "scan", wifi.ip(),
                 static_cast<unsigned long>(g_row.waitedMs));
  Serial1.printf("  the panel has said LISTENING for %lu ms of that\n",
                 static_cast<unsigned long>((esp_timer_get_time() - g_releaseUs) / 1000));

  g_row.sentBytes = static_cast<uint32_t>(audio.wavBytes());

  const StickyBackend::Result result = backend.ask(url, audio.wav(), audio.wavBytes());

  g_row.roundTripMs = static_cast<uint16_t>(backend.elapsedMs());
  g_row.status = static_cast<int16_t>(backend.status());

  Serial1.printf("  round trip: %lu ms, first byte at %lu ms",
                 static_cast<unsigned long>(backend.elapsedMs()),
                 static_cast<unsigned long>(backend.firstByteMs()));
  // Bytes per millisecond is kilobytes per second, and for anything that got a
  // status back the wait is almost all upload -- which is the number S8 needs.
  if (backend.status() != 0 && backend.firstByteMs() != 0) {
    Serial1.printf(" (%lu KB/s up)",
                   static_cast<unsigned long>(audio.wavBytes() / backend.firstByteMs()));
  }
  if (result == StickyBackend::Result::Ok) {
    Serial1.printf(", %lu bytes of JSON\n", static_cast<unsigned long>(backend.bodyBytes()));
    Serial1.printf("  answer: \"%s\"\n", backend.answer());
  } else {
    Serial1.printf("\n  failed: %s\n", backend.lastError());
  }

  // The vision's error table, in the one place that owns it. **Every outcome
  // chirps before it draws**, answers included: the buzzer is the channel that
  // is not one to two seconds behind, and a full refresh is legible long before
  // it finishes -- the text appears inverted partway through and can already be
  // read. A chirp held back until the refresh returns lands after the user has
  // looked, so it reads as extra latency rather than as the answer arriving.
  switch (result) {
    case StickyBackend::Result::Ok:
      stickyBuzzer::answer();
      screen.answer(backend.answer());
      finish(Outcome::Answered, true);

    case StickyBackend::Result::NoServer:
      stickyBuzzer::error();
      screen.error("NO SERVER", nullptr);
      finish(Outcome::NoServer, true);

    case StickyBackend::Result::ServerError: {
      char detail[16];
      snprintf(detail, sizeof(detail), "%d", backend.status());
      stickyBuzzer::error();
      screen.error("SERVER ERROR", detail);
      finish(Outcome::ServerError, true);
    }

    case StickyBackend::Result::BadResponse:
      stickyBuzzer::error();
      screen.error("BAD RESPONSE", nullptr);
      finish(Outcome::BadResponse, true);

    case StickyBackend::Result::TimedOut:
      stickyBuzzer::error();
      screen.error("TIMED OUT", nullptr);
      finish(Outcome::TimedOut, true);
  }

  finish(Outcome::Broken, false);
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
