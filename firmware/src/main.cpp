// S7b -- Cached DHCP lease. A temporary driver, the way S1's to S7's were:
// docs/implementation.md asks for E6's numbers coming back from the firmware
// rather than from a rig, and for a question that starts with a stale lease
// still getting its answer. S8 replaces all of this with the orchestrator.
//
// It is S7's driver with the error table taken out -- that walk is done and
// recorded -- and the address put in its place. Each press walks one row and
// the next press walks the next:
//
//   1  nothing cached yet             DHCP, ~3.3 s from the top of setup()
//   2  the last question's lease      installed, ~200 ms and no client at all
//   3  a closed port                  NO SERVER at once, and the lease kept
//   4  the lease, still there         ~200 ms again -- the refusal changed nothing
//   5  a lease from another network   two dead connects, DHCP, then the answer
//
// **After the last row every press behaves like row 2**, which is what the
// device does from then on. The counter lives in RTC memory, which the reset
// that flashing or opening the monitor causes clears -- so a session always
// starts at row 1 and a walk of the table is five presses from a fresh flash.
//
// Row 3 is the row that says the detector is a detector rather than "any
// NO SERVER": a refusal proves something is at the address, so the address is
// fine and the lease must survive it. Row 5 is the one failure in this step
// that cannot be reached by waiting -- a lease only goes stale when the network
// changes underneath it -- so the driver makes one, through
// StickyWifi::spoilLease(), on the wake before.
//
// A press too short to be a question is discarded without consuming a row, and
// so is a question that never reached the POST.

#include <Arduino.h>
#include <esp_timer.h>
#include <stdio.h>
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

// Where the address this question ran on came from.
enum class Mode : uint8_t {
  Dhcp,   // asked for and waited on, which is what every question cost before
  Lease,  // the last question's, installed in 40 ms
  Stale,  // ... the same, after the driver moved it onto another network
};

struct Plan {
  const char* what;      // what this press is testing
  const char* expected;  // what E6 says should come of it
  bool closedPort;       // ask a port nothing is listening on instead
  bool spoilAfter;       // leave the next wake a lease from the wrong network
};

constexpr Plan kPlans[] = {
    {"the first question of a session", "DHCP -- about 3.3 s to a usable network", false, false},
    {"a question carrying the last one's lease", "the lease installed -- about 200 ms, no client",
     false, false},
    {"a refusal, which is not a stale lease", "NO SERVER at once, and the lease kept", true,
     false},
    {"the lease, still there after the refusal", "about 200 ms again -- nothing was dropped",
     false, true},
    {"a lease from a network the device is not on",
     "two connects that answer nothing, then DHCP, then the answer", false, false},
};
constexpr uint32_t kPlanCount = sizeof(kPlans) / sizeof(kPlans[0]);

// Which row a press that is past the table walks: the one that is the device's
// ordinary behaviour from then on.
constexpr uint32_t kSteadyPlan = 1;

// The log lives in RTC memory, which survives deep sleep but not a power cycle
// or a reset -- so a session is exactly what this table holds.
constexpr uint32_t kLogRows = 16;
constexpr uint32_t kMagic = 0x57B0FFEE;

struct Row {
  uint8_t plan;
  uint8_t mode;
  uint8_t outcome;
  uint16_t heldMs;
  uint16_t linkMs;      // begin() to the link, before the address
  uint16_t connectMs;   // begin() to a usable network
  uint16_t bootMs;      // the top of setup() to the same moment -- E6's column
  uint16_t waitedMs;    // release to it, 0 when it was already up
  uint16_t toPostMs;    // release to the upload starting, which is not the same
  uint16_t roundTripMs; // the ask that produced the outcome
  uint16_t lostMs;      // the connects that answered nothing, before the drop
  uint16_t renewMs;     // DHCP after the lease was dropped, 0 when none was
  uint16_t answeredMs;  // release to the answer being on the glass
  uint32_t sentBytes;
  int16_t status;
};

RTC_DATA_ATTR uint32_t g_magic;
RTC_DATA_ATTR uint32_t g_walked;     // rows of the table walked so far
RTC_DATA_ATTR uint32_t g_questions;  // rows written, which may exceed kLogRows
RTC_DATA_ATTR bool g_spoiled;        // the lease this wake found was made stale
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

// The three moments the question is measured against, and not one of them is
// the moment this thread reaches them. The panel refresh is why: it takes two
// and a half seconds during which nothing here polls anything, and every number
// that was read off a poll came back with the refresh inside it.
//
//  * The release is tEntry plus the hold, because the orchestrator can be
//    inside that refresh when the button comes up -- the capture task sees it,
//    this thread does not.
//  * Online is begin() plus StickyWifi::onlineMs(), which is the WiFi task's
//    own timestamp of the address arriving. The first run of this driver used
//    the first poll that reported it and measured the panel instead: four wakes
//    reported an installed lease as 2642, 2643, 2642, 2643 ms.
//  * begin() is taken here rather than asked for, because everything above is
//    relative to it.
//
// All three are zero until they happen.
int64_t g_releaseUs = 0;
int64_t g_beginUs = 0;
int64_t g_onlineUs = 0;

// The row this question walks: the next one while the table is unfinished, and
// the steady one once it has been walked through.
uint32_t planIndex() { return g_walked < kPlanCount ? g_walked : kSteadyPlan; }

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

const char* modeName(uint8_t mode) {
  switch (static_cast<Mode>(mode)) {
    case Mode::Dhcp: return "dhcp";
    case Mode::Lease: return "lease";
    case Mode::Stale: return "stale";
    default: return "?";
  }
}

// StickyWifi hands addresses back in IPAddress's byte order -- first octet in
// the low byte -- so that its header does not have to include WiFi.h.
void ipText(char* out, size_t size, uint32_t address) {
  snprintf(out, size, "%u.%u.%u.%u", static_cast<unsigned>(address & 0xFF),
           static_cast<unsigned>((address >> 8) & 0xFF),
           static_cast<unsigned>((address >> 16) & 0xFF),
           static_cast<unsigned>((address >> 24) & 0xFF));
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

void planUrl(char* out, size_t size, const Plan& plan) {
  if (plan.closedPort) {
    urlWithPort(out, size, 1, config::kAudioPath);
  } else {
    snprintf(out, size, "%s%s", config::kBackendBaseUrl, config::kAudioPath);
  }
}

void printTable() {
  static constexpr const char* kFormat =
      "%3s  %-6s %7s %9s %8s %7s %8s %10s %9s %8s %8s  %s\n";

  Serial1.println();
  Serial1.printf(kFormat, "#", "mode", "link", "connect", "boot", "wait", "to POST", "wav",
                 "round trip", "lost", "dhcp", "outcome");

  const uint32_t rows = g_questions < kLogRows ? g_questions : kLogRows;
  for (uint32_t i = 0; i < rows; ++i) {
    const Row& row = g_log[i];

    char index[8];
    char link[12] = "--";
    char connect[12] = "--";
    char boot[12] = "--";
    char wait[12] = "--";
    char toPost[12] = "--";
    char sent[12] = "--";
    char roundTrip[12] = "--";
    char lost[12] = "--";
    char renew[12] = "--";
    char outcome[32];

    snprintf(index, sizeof(index), "%lu", static_cast<unsigned long>(i + 1));
    if (row.linkMs != 0) snprintf(link, sizeof(link), "%u ms", row.linkMs);
    if (row.connectMs != 0) snprintf(connect, sizeof(connect), "%u ms", row.connectMs);
    if (row.bootMs != 0) snprintf(boot, sizeof(boot), "%u ms", row.bootMs);
    if (row.heldMs != 0) snprintf(wait, sizeof(wait), "%u ms", row.waitedMs);
    if (row.toPostMs != 0) snprintf(toPost, sizeof(toPost), "%u ms", row.toPostMs);
    if (row.sentBytes != 0) snprintf(sent, sizeof(sent), "%lu B",
                                     static_cast<unsigned long>(row.sentBytes));
    if (row.roundTripMs != 0) snprintf(roundTrip, sizeof(roundTrip), "%u ms", row.roundTripMs);
    if (row.lostMs != 0) snprintf(lost, sizeof(lost), "%u ms", row.lostMs);
    if (row.renewMs != 0) snprintf(renew, sizeof(renew), "%u ms", row.renewMs);

    if (static_cast<Outcome>(row.outcome) == Outcome::ServerError) {
      snprintf(outcome, sizeof(outcome), "SERVER ERROR %d", static_cast<int>(row.status));
    } else {
      snprintf(outcome, sizeof(outcome), "%s", outcomeName(row.outcome));
    }

    Serial1.printf(kFormat, index, modeName(row.mode), link, connect, boot, wait, toPost, sent,
                   roundTrip, lost, renew, outcome);
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

// The one exit. `consumed` says whether the row this question was walking has
// been walked -- a tap or a failure before the POST leaves it for the next
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

  if (consumed) {
    // The row is only spoiled once the question that was testing it is over,
    // and only when it actually ran: a tap leaves the table where it was, and
    // spoiling on the way past would put the next wake on a row it was not
    // walking.
    if (kPlans[g_row.plan].spoilAfter) g_spoiled = StickyWifi::spoilLease();
    ++g_walked;
  }

  printTable();

  mic.end();
  wifi.end();
  waitForRelease();

  Serial1.printf("  press the AI button for \"%s\"\n", kPlans[planIndex()].what);
  Serial1.flush();

  stickyPower::deepSleep();
}

// A failure with nothing sent: log it, show it, chirp, sleep. The row stays
// where it is.
[[noreturn]] void fail(Outcome outcome, const char* title, const char* detail) {
  Serial1.printf("  %s: %s\n", title, detail != nullptr ? detail : "");
  stickyBuzzer::error();
  if (screen.begin()) screen.error(title, detail);
  finish(outcome, false);
}

// The question, with S7b's rule around it.
//
// **A connect that nothing answered is the only thing that can say a cached
// lease has gone stale**, because a lease that has outlived its network installs
// exactly as well as a good one and fails only when a packet needs to go
// somewhere. So it is read that way -- but not on the first one. S7 found a
// healthy network on this desk producing a connect that fails outright about
// once in fifteen questions, and E7 went looking for the mechanism and could not
// reproduce it; a lease dropped on one of those costs 3.2 s on a wake where
// nothing was wrong. Asking twice costs one connect timeout on a wake that was
// already going to be slow, which is the cheaper of the two mistakes.
//
// Everything else the backend can do -- refuse the connection, answer 500,
// answer nothing, answer nonsense -- proves there is something at the address
// and therefore that the address works, and leaves the lease alone.
StickyBackend::Result askAbout(const char* url) {
  StickyBackend::Result result = backend.ask(url, audio.wav(), audio.wavBytes());
  g_row.roundTripMs = static_cast<uint16_t>(backend.elapsedMs());

  const bool couldBeStale =
      result == StickyBackend::Result::NoServer && backend.unreachable() && wifi.usedLease();
  if (!couldBeStale) return result;

  Serial1.printf("  nothing answered in %lu ms, and this question is on a reused address"
                 " -- asking once more before believing it\n",
                 static_cast<unsigned long>(backend.elapsedMs()));
  g_row.lostMs = static_cast<uint16_t>(backend.elapsedMs());

  result = backend.ask(url, audio.wav(), audio.wavBytes());
  g_row.roundTripMs = static_cast<uint16_t>(backend.elapsedMs());
  g_row.lostMs = static_cast<uint16_t>(g_row.lostMs + backend.elapsedMs());
  if (result != StickyBackend::Result::NoServer || !backend.unreachable()) {
    Serial1.println("  the second connect got somewhere -- the lease was not the problem");
    return result;
  }

  Serial1.println("  twice, so the address is the suspect -- dropping the lease and asking DHCP");
  const uint32_t stale = wifi.ipv4();
  wifi.renewAddress();
  while (wifi.poll() == StickyWifi::State::Connecting) delay(kPollMs);
  g_row.renewMs = static_cast<uint16_t>(wifi.renewMs());

  if (!wifi.online()) {
    Serial1.printf("  no address after %lu ms: %s\n", static_cast<unsigned long>(wifi.renewMs()),
                   wifi.lastError());
    fail(Outcome::NoWifi, "NO WIFI", nullptr);
  }

  // A server usually hands back the address it handed out before, and when it
  // does the lease was never the problem: the question is about to fail again
  // for whatever reason it failed the first time, and the 3.2 s just spent was
  // the price of a false positive. Worth saying out loud, because it is the one
  // line in the log that tells the two apart.
  char before[16];
  ipText(before, sizeof(before), stale);
  Serial1.printf("  DHCP took %lu ms and handed back %s -- %s\n",
                 static_cast<unsigned long>(wifi.renewMs()), wifi.ip(),
                 static_cast<uint32_t>(wifi.ipv4()) == stale
                     ? "the same address, so the lease was not what was wrong"
                     : "a different address, so it was");

  result = backend.ask(url, audio.wav(), audio.wavBytes());
  g_row.roundTripMs = static_cast<uint16_t>(backend.elapsedMs());
  return result;
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
    g_spoiled = false;
  }

  const uint32_t index = planIndex();
  const Plan& plan = kPlans[index];
  const bool spoiled = g_spoiled;
  g_spoiled = false;

  g_row = Row{};
  g_row.plan = static_cast<uint8_t>(index);

  char url[StickyBackend::kMaxUrlChars];
  planUrl(url, sizeof(url), plan);

  Serial1.println();
  Serial1.printf("S7b lease driver -- question %lu, wake %s, reset %s\n",
                 static_cast<unsigned long>(g_questions + 1), stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());
  Serial1.printf("  row %lu/%lu: %s -- %s\n", static_cast<unsigned long>(index + 1),
                 static_cast<unsigned long>(kPlanCount), plan.what, url);
  Serial1.printf("  expecting %s\n", plan.expected);

  StickyWifi::Lease lease;
  if (StickyWifi::cachedLease(lease)) {
    char ip[16];
    char gateway[16];
    ipText(ip, sizeof(ip), lease.ip);
    ipText(gateway, sizeof(gateway), lease.gateway);
    Serial1.printf("  lease in RTC memory: %s via %s, %lu s old of %lu%s\n", ip, gateway,
                   static_cast<unsigned long>(lease.ageS),
                   static_cast<unsigned long>(lease.seconds),
                   spoiled ? " -- moved onto another network on purpose" : "");
  } else {
    Serial1.println("  no lease in RTC memory -- this question pays for DHCP");
  }

  // The microphone comes first and the chirp comes after it: the chirp means
  // "the microphone is live", so nothing that can delay capture may sit between
  // them. E1 and E3, and the vision's step 3.
  if (!mic.begin()) fail(Outcome::Broken, "NO MICROPHONE", mic.lastError());
  if (!audio.begin(mic.sampleRate())) fail(Outcome::Broken, "NO MEMORY", audio.lastError());
  if (!capture.start(mic, audio, button)) {
    fail(Outcome::Broken, "NO MICROPHONE", "the capture task would not start");
  }
  stickyBuzzer::ready();

  g_beginUs = esp_timer_get_time();
  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword)) {
    capture.abort();
    capture.wait();
    fail(Outcome::NoWifi, "NO WIFI", wifi.lastError());
  }
  g_row.mode = static_cast<uint8_t>(wifi.usedLease() ? (spoiled ? Mode::Stale : Mode::Lease)
                                                     : Mode::Dhcp);
  if (wifi.hadLease() && !wifi.usedLease()) {
    Serial1.println("  the lease was not installed -- past half the life the server gave it,"
                    " or WiFi.config() refused it");
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
    if (wifi.poll() == StickyWifi::State::Failed) capture.abort();
    delay(kPollMs);
  }

  mic.end();

  g_row.heldMs = static_cast<uint16_t>(button.heldMs());
  g_releaseUs = tEntry + static_cast<int64_t>(button.heldMs()) * 1000;

  Serial1.printf("  recording: %s after %lu ms held -- %lu ms of audio, %lu bytes to send\n",
                 capture.stopName(), static_cast<unsigned long>(button.heldMs()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(audio.wavBytes()));

  if (capture.stop() == StickyCapture::Stop::ReadFailed) {
    fail(Outcome::Broken, "NO MICROPHONE", capture.lastError());
  }
  if (button.isTap()) {
    Serial1.println("  too short to be a question -- nothing sent, the row is unchanged");
    finish(Outcome::Tap, false);
  }

  // A release that arrives before the address does is not a failure: the
  // association has a budget of its own and NO WIFI is what happens when that
  // runs out. S7 found this the hard way and the note is there.
  while (wifi.poll() == StickyWifi::State::Connecting) delay(kPollMs);

  if (!wifi.online()) {
    Serial1.printf("  no network after %lu ms: %s\n", static_cast<unsigned long>(wifi.elapsedMs()),
                   wifi.lastError());
    fail(Outcome::NoWifi, "NO WIFI", nullptr);
  }

  g_onlineUs = g_beginUs + static_cast<int64_t>(wifi.onlineMs()) * 1000;
  if (g_onlineUs > g_releaseUs) {
    g_row.waitedMs = static_cast<uint16_t>((g_onlineUs - g_releaseUs) / 1000);
  }

  g_row.linkMs = static_cast<uint16_t>(wifi.linkMs());
  g_row.connectMs = static_cast<uint16_t>(wifi.onlineMs());
  g_row.bootMs = static_cast<uint16_t>((g_onlineUs - tEntry) / 1000);

  // The two numbers E6 put side by side, now from the firmware: a wake that
  // asks for an address against a wake that already has one, measured from the
  // top of setup() both times. The third number is what the thread doing the
  // asking believed, which is the panel refresh and not the network.
  Serial1.printf("  online in %lu ms (%s AP, %s address), %s -- link at %lu ms, %lu ms from the"
                 " top of setup(), %lu ms waited for after the release, first noticed at %lu ms\n",
                 static_cast<unsigned long>(wifi.onlineMs()),
                 wifi.usedCache() ? "cached" : "scanned", wifi.usedLease() ? "installed" : "leased",
                 wifi.ip(), static_cast<unsigned long>(wifi.linkMs()),
                 static_cast<unsigned long>(g_row.bootMs),
                 static_cast<unsigned long>(g_row.waitedMs),
                 static_cast<unsigned long>(wifi.elapsedMs()));

  // The one way the cache can fail without anything looking wrong: DHCP ran,
  // the address is fine, and the client never said how long it lives -- so
  // there is nothing to age an entry against and none is written. It would show
  // up only as every question paying for DHCP forever, which is the state this
  // step exists to leave behind.
  if (!wifi.usedLease() && wifi.leaseSeconds() == 0) {
    Serial1.println("  the client did not say how long the lease lives -- nothing cached for the"
                    " next question");
  }

  g_row.sentBytes = static_cast<uint32_t>(audio.wavBytes());

  // **The network stops being what the question waits for, and the panel
  // starts.** With the address installed the network is usable 200 ms into the
  // wake, but this thread cannot act on that until the LISTENING refresh
  // returns -- so a question shorter than the refresh reaches the upload when
  // the panel lets it, not when the radio does. The gap between this number and
  // `wait` is the whole of S8's open question, in one column.
  g_row.toPostMs = static_cast<uint16_t>((esp_timer_get_time() - g_releaseUs) / 1000);
  Serial1.printf("  %lu ms from the release to the upload starting, of which %lu ms was the"
                 " network\n",
                 static_cast<unsigned long>(g_row.toPostMs),
                 static_cast<unsigned long>(g_row.waitedMs));

  const StickyBackend::Result result = askAbout(url);

  g_row.status = static_cast<int16_t>(backend.status());

  Serial1.printf("  round trip: %lu ms, first byte at %lu ms",
                 static_cast<unsigned long>(backend.elapsedMs()),
                 static_cast<unsigned long>(backend.firstByteMs()));
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

  // The vision's error table, in the one place that owns it. Every outcome
  // chirps before it draws, answers included -- S7's note has why.
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
