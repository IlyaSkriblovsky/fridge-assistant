// S8 -- The flow. The orchestrator, and the end of the temporary drivers that
// S1 to S7b were: wake, latch, microphone, capture task, ready chirp, WiFi and
// the Listening screen, release, taken chirp, the working screen, upload,
// answer or error chirp, draw, deep sleep.
//
// **The order at the front is load-bearing and measured.** Capture starts
// before the chirp, because the chirp means "the microphone is live" and a
// chirp that came first would put its own duration into the dead time at the
// front of every question -- E1 and E3, and the vision's step 3. Nothing may be
// inserted between mic.begin() and stickyBuzzer::ready() that can block.
//
// **The panel is the largest number between the wake and the upload**, at 2.4 s
// against 300 ms of network and 500 ms of round trip (S5, S7b, E7). Two things
// here follow from that and from nothing else:
//
//  * The pre-clear is a cold-start thing. A full white frame before the first
//    real screen is what a controller whose previous-image RAM has nothing to
//    do with the glass needs; on a wake it is 2373 ms spent flushing a panel
//    the Listening screen overwrites anyway.
//  * The working screen is a partial refresh -- S8 settled the vision's step 6
//    that way, because it is the one transition the user waits through and a
//    full refresh would put 2.4 s in front of a round trip that is usually half
//    a second. The chirp goes in front of it for the same reason every other
//    chirp does: the buzzer exists because the panel is late.
//
// **Every exit is deep sleep with the latch held**, the error paths and the
// discarded tap included, and there is exactly one of them: finish().
//
// Two things came across from the S7b driver rather than dying with it. The
// stale-lease rule is askRenewingStaleLease() below -- ask again, then drop the
// lease, take an address and ask once more -- and it lives here because it
// spans Backend and WifiLink and neither half can see it alone. The pre-clear
// branch on stickyPower::wokeFromDeepSleep() is the other.

#include <Arduino.h>
#include <esp_timer.h>
#include <stdio.h>

#include "secrets.h"

#include "sticky/button.h"
#include "sticky/buzzer.h"
#include "sticky/mic.h"
#include "sticky/power.h"
#include "sticky/screen.h"

#include "backend.h"
#include "capture.h"
#include "recording.h"
#include "wifi_link.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// How often the association and the capture task are looked at while the button
// is held. Small, because it is also the resolution of WifiLink::elapsedMs().
constexpr uint32_t kPollMs = 10;

// A press held through the log would wake the board again the moment it goes to
// sleep, since ext1 wakes on the level rather than on an edge. Only the 30 s
// recording cap can reach the exit with the button still down.
constexpr uint32_t kReleaseWaitMs = 30000;

// How long finish() gives the capture task to notice an abort before it stops
// waiting. Bounded rather than indefinite for one case: a start() that created
// the semaphore and then failed to create the task leaves nothing to give it,
// and a question must not end by hanging with the rail latched. A chunk is
// 16 ms, so an abort that is going to land has landed long before this.
constexpr uint32_t kCaptureJoinMs = 500;

// What the question came to. The first five are Backend's results, the rest are
// the ways one ends before there is anything to send.
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

StickyButton button;
StickyMic mic;
Recording audio;
Capture capture;
WifiLink wifi;
StickyScreen screen;
Backend backend;

// The moments the question is measured against, and two of them are not the
// moment this thread reaches them. The panel refresh is why: it takes two and a
// half seconds during which nothing here polls anything, so a number read off a
// poll would have the refresh inside it.
//
//  * The release is the top of setup() plus the hold, because this thread can
//    be inside the Listening refresh when the button comes up -- the capture
//    task sees it, this thread does not.
//  * Online is wifi.begin() plus WifiLink::onlineMs(), which is the WiFi task's
//    own timestamp of the address arriving.
//
// All of them are zero until they happen.
int64_t g_entryUs = 0;
int64_t g_releaseUs = 0;
int64_t g_beginUs = 0;
int64_t g_listeningEndUs = 0;

// What the panel cost this question, which is the one thing S8 set out to
// measure. Kept apart from the rest because it is the reason the working screen
// has the shape it has, and because D4's display task is weighed against it.
uint32_t g_listeningMs = 0;
uint32_t g_workingMs = 0;
uint32_t g_answerMs = 0;

// The release to the moment the last chirp starts, which is the whole of what
// the user waits through: the refresh behind it is readable long before it ends
// and the chirp is what says to look up. Measured rather than derived, because
// the chirp itself is 230 ms and putting it on the panel's side of the line
// would report every full refresh in this firmware as 245 ms slower than
// S5 measured the same screen at.
uint32_t g_toChirpMs = 0;

// **The part of the Listening refresh that the question waited for**, which is
// how much of it was still running when the button came up. It is zero for a
// hold longer than the refresh and the largest term in the wait for anything
// shorter -- 1660 ms of a 1.3 s question, measured -- so it belongs in the
// breakdown beside the network and the round trip rather than in the remainder.
uint32_t g_listeningLeftMs = 0;

const char* outcomeName(Outcome outcome) {
  switch (outcome) {
    case Outcome::Answered: return "answered";
    case Outcome::NoServer: return "NO SERVER";
    case Outcome::ServerError: return "SERVER ERROR";
    case Outcome::BadResponse: return "BAD RESPONSE";
    case Outcome::TimedOut: return "TIMED OUT";
    case Outcome::NoWifi: return "NO WIFI";
    case Outcome::Broken: return "broken";
    case Outcome::Tap: return "tap, discarded";
  }
  return "?";
}

// WifiLink hands addresses back in IPAddress's byte order -- first octet in the
// low byte -- so that its header does not have to include WiFi.h.
void ipText(char* out, size_t size, uint32_t address) {
  snprintf(out, size, "%u.%u.%u.%u", static_cast<unsigned>(address & 0xFF),
           static_cast<unsigned>((address >> 8) & 0xFF),
           static_cast<unsigned>((address >> 16) & 0xFF),
           static_cast<unsigned>((address >> 24) & 0xFF));
}

uint32_t sinceReleaseMs() {
  if (g_releaseUs == 0) return 0;
  return static_cast<uint32_t>((esp_timer_get_time() - g_releaseUs) / 1000);
}

// What the question cost the user, which is the release to the answer chirp --
// the refresh after it is time the panel is readable through, not time spent
// waiting. Printed as its parts because each of them belongs to a different
// decision: the panel to this step and D6, the network to S7b, the round trip
// to E7 and D4.
//
// **The parts are the chain and not the calendar.** Everything here is time
// this thread spent in one thing after the release, in the order it spent it,
// so the four add up to the total bar the chirp and the logging. The address
// arriving is deliberately not one of them: it happens in the WiFi task and
// usually lands underneath the Listening refresh, so counting it from the
// release would count the same milliseconds twice. What is counted is the wait
// that was left once the panel let go of this thread, which is nothing at all
// on a wake with a lease -- the "online in" line above has the radio's own view.
void logTiming(Outcome outcome, uint32_t networkWaitMs, uint32_t roundTripMs) {
  if (g_releaseUs == 0) return;

  Serial1.printf("  release to the %s: %lu ms -- %lu ms left of the Listening refresh,"
                 " %lu ms working screen, %lu ms waiting for the network, %lu ms round"
                 " trip; %lu ms of %s refresh after it\n",
                 outcome == Outcome::Answered ? "answer chirp" : "error chirp",
                 static_cast<unsigned long>(g_toChirpMs),
                 static_cast<unsigned long>(g_listeningLeftMs),
                 static_cast<unsigned long>(g_workingMs),
                 static_cast<unsigned long>(networkWaitMs),
                 static_cast<unsigned long>(roundTripMs),
                 static_cast<unsigned long>(g_answerMs),
                 screen.lastWasPartial() ? "partial" : "full");
}

// A press held through the log is still down when the sleep is armed, and ext1
// wakes on the level.
void waitForRelease() {
  if (!button.isDown()) return;

  const int64_t deadlineUs = esp_timer_get_time() + kReleaseWaitMs * 1000LL;
  Serial1.println("  waiting for the button to come back up");
  while (button.isDown() && esp_timer_get_time() < deadlineUs) delay(5);
}

// The one exit. Called straight after whichever screen was drawn, so everything
// above it has already happened.
[[noreturn]] void finish(Outcome outcome) {
  // The capture task owns the microphone until it is joined, so the abort comes
  // before mic.end() and not after it. Both are no-ops when the task never
  // started or has already reported.
  capture.abort();
  capture.wait(kCaptureJoinMs);
  mic.end();
  wifi.end();

  Serial1.printf("  %s, %lu ms awake\n", outcomeName(outcome),
                 static_cast<unsigned long>((esp_timer_get_time() - g_entryUs) / 1000));

  waitForRelease();
  Serial1.flush();

  stickyPower::deepSleep();
}

// A failure: log it, chirp, show it, sleep. The chirp comes before the screen
// for the reason every chirp does -- the panel is one to two seconds behind and
// holding the sound back spends that advantage.
[[noreturn]] void fail(Outcome outcome, const char* title, const char* detail) {
  Serial1.printf("  %s: %s\n", title, detail != nullptr ? detail : "");

  g_toChirpMs = sinceReleaseMs();
  stickyBuzzer::error();

  const int64_t refreshStartUs = esp_timer_get_time();
  const bool drawn = screen.begin();
  if (drawn) screen.error(title, detail);
  g_answerMs = static_cast<uint32_t>((esp_timer_get_time() - refreshStartUs) / 1000);
  if (drawn) {
    Serial1.printf("  error screen: %lu ms %s\n", static_cast<unsigned long>(g_answerMs),
                   screen.lastWasPartial() ? "partial" : "full");
  }

  finish(outcome);
}

// The question, with S7b's stale-lease rule around it.
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
Backend::Result askRenewingStaleLease() {
  Backend::Result result = backend.ask(audio.wav(), audio.wavBytes());

  const bool couldBeStale =
      result == Backend::Result::NoServer && backend.unreachable() && wifi.usedLease();
  if (!couldBeStale) return result;

  Serial1.printf("  nothing answered in %lu ms, and this question is on a reused address"
                 " -- asking once more before believing it\n",
                 static_cast<unsigned long>(backend.elapsedMs()));

  result = backend.ask(audio.wav(), audio.wavBytes());
  if (result != Backend::Result::NoServer || !backend.unreachable()) {
    Serial1.println("  the second connect got somewhere -- the lease was not the problem");
    return result;
  }

  Serial1.println("  twice, so the address is the suspect -- dropping the lease and asking DHCP");
  const uint32_t stale = wifi.ipv4();
  wifi.renewAddress();
  while (wifi.poll() == WifiLink::State::Connecting) delay(kPollMs);

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

  return backend.ask(audio.wav(), audio.wavBytes());
}

}  // namespace

void setup() {
  g_entryUs = esp_timer_get_time();

  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();
  button.begin(g_entryUs);  // takes GPIO4 back from the RTC pad and pulls it up

  // No settling delay after it. The delay(50) that used to follow came with the
  // first demo, waited for nothing, and was a third of the wake-to-chirp time
  // -- S9 measured it on the firmware's own image.
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);

  Serial1.println();
  Serial1.printf("question -- wake %s, reset %s\n", stickyPower::wakeupCauseName(),
                 stickyPower::resetReasonName());

  WifiLink::Lease lease;
  if (WifiLink::cachedLease(lease)) {
    char ip[16];
    char gateway[16];
    ipText(ip, sizeof(ip), lease.ip);
    ipText(gateway, sizeof(gateway), lease.gateway);
    Serial1.printf("  lease in RTC memory: %s via %s, %lu s old of %lu\n", ip, gateway,
                   static_cast<unsigned long>(lease.ageS),
                   static_cast<unsigned long>(lease.seconds));
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
  // Timed as the chirp starts, not as it returns: the chirp is 110 ms of
  // blocking, and a timestamp taken after it reports the end of the chirp as the
  // moment the microphone went live -- which is how S8 came to record 203 ms for
  // a chirp that started at 93. Measured from the top of setup(), so the boot is
  // not in it; S9 has the boot.
  const int64_t chirpUs = esp_timer_get_time();
  stickyBuzzer::ready();
  Serial1.printf("  ready chirp %lu ms into setup(), with the recording already running\n",
                 static_cast<unsigned long>((chirpUs - g_entryUs) / 1000));

  g_beginUs = esp_timer_get_time();
  if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword)) {
    fail(Outcome::NoWifi, "NO WIFI", wifi.lastError());
  }

  // The one failure with nothing to draw a message on: chirp, log and sleep.
  if (!screen.begin()) {
    Serial1.printf("  the panel would not start: %s\n", screen.lastError());
    stickyBuzzer::error();
    finish(Outcome::Broken);
  }

  // A cold start is the only time the controller's previous-image RAM has
  // nothing to do with what is on the glass.
  if (!stickyPower::wokeFromDeepSleep()) {
    const int64_t startUs = esp_timer_get_time();
    screen.clear();
    Serial1.printf("  cold start: %lu ms of pre-clear\n",
                   static_cast<unsigned long>((esp_timer_get_time() - startUs) / 1000));
  }

  {
    const int64_t startUs = esp_timer_get_time();
    screen.listening();
    g_listeningEndUs = esp_timer_get_time();
    g_listeningMs = static_cast<uint32_t>((g_listeningEndUs - startUs) / 1000);
  }

  // Everything from here until the release is the capture task's; this loop
  // only watches. A drop is the vision's rule that a recording with nowhere to
  // go is aborted rather than finished.
  while (!capture.finished()) {
    if (wifi.poll() == WifiLink::State::Failed) capture.abort();
    delay(kPollMs);
  }

  mic.end();

  g_releaseUs = g_entryUs + static_cast<int64_t>(button.heldMs()) * 1000;

  // A hold shorter than the refresh ends inside it, and the rest of the refresh
  // is then the first thing the question waits for -- this thread cannot act on
  // the release until it returns. Both moments are the ones they happened at
  // rather than the ones this thread noticed, which is why neither is a poll.
  if (g_listeningEndUs > g_releaseUs) {
    g_listeningLeftMs = static_cast<uint32_t>((g_listeningEndUs - g_releaseUs) / 1000);
  }

  Serial1.printf("  recording: %s after %lu ms held -- %lu ms of audio, %lu bytes to send\n",
                 capture.stopName(), static_cast<unsigned long>(button.heldMs()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(audio.wavBytes()));
  Serial1.printf("  Listening screen: %lu ms, and the release was %s it%s\n",
                 static_cast<unsigned long>(g_listeningMs),
                 g_listeningLeftMs != 0 ? "inside" : "after",
                 g_listeningLeftMs != 0 ? " -- the question waits out the rest" : "");

  if (capture.stop() == Capture::Stop::ReadFailed) {
    fail(Outcome::Broken, "NO MICROPHONE", capture.lastError());
  }

  // Silent on purpose -- D7. The chirp below is the first sound a question
  // makes after the ready chirp, and a tap must not make it.
  if (button.isTap()) {
    Serial1.println("  too short to be a question -- nothing sent");
    finish(Outcome::Tap);
  }

  // The vision's step 6, in two parts. The chirp says the question was taken
  // and costs milliseconds; the screen says the same thing to somebody who was
  // not listening, and costs whatever a partial refresh costs on this panel --
  // which is the number S8 exists to find out.
  stickyBuzzer::taken();
  {
    const int64_t startUs = esp_timer_get_time();
    const bool drawn = screen.working();
    g_workingMs = static_cast<uint32_t>((esp_timer_get_time() - startUs) / 1000);
    if (drawn) {
      Serial1.printf("  working screen: %lu ms partial\n",
                     static_cast<unsigned long>(g_workingMs));
    } else {
      // The panel keeps reading LISTENING until the answer lands, which is what
      // the question would have looked like without this screen at all.
      Serial1.printf("  working screen refused after %lu ms: %s\n",
                     static_cast<unsigned long>(g_workingMs), screen.lastError());
    }
  }

  // A release that arrives before the address does is not a failure: the
  // association has a budget of its own and NO WIFI is what happens when that
  // runs out.
  //
  // Timed from here rather than from the release on purpose: this is the poll
  // loop's own view and the question it answers is the poller's -- how much of
  // the wait was left once the panel let go. The radio's view of the same
  // moment is wifi.onlineMs(), two lines down.
  const int64_t networkWaitStartUs = esp_timer_get_time();
  while (wifi.poll() == WifiLink::State::Connecting) delay(kPollMs);
  const uint32_t networkWaitMs =
      static_cast<uint32_t>((esp_timer_get_time() - networkWaitStartUs) / 1000);

  if (!wifi.online()) {
    Serial1.printf("  no network after %lu ms: %s\n", static_cast<unsigned long>(wifi.elapsedMs()),
                   wifi.lastError());
    fail(Outcome::NoWifi, "NO WIFI", nullptr);
  }

  // Timed by the WiFi task rather than by this one, because this one spent the
  // Listening refresh not polling anything -- see WifiLink::onlineMs(). It is
  // the radio's answer to "when was there a network", and it is not the same
  // question as networkWaitMs above: an address that lands 2.6 s after the
  // release lands under the refresh and costs the question nothing.
  const int64_t onlineUs = g_beginUs + static_cast<int64_t>(wifi.onlineMs()) * 1000;
  const uint32_t addressAfterReleaseMs =
      onlineUs > g_releaseUs ? static_cast<uint32_t>((onlineUs - g_releaseUs) / 1000) : 0;

  Serial1.printf("  online in %lu ms (%s AP, %s address), %s -- %lu ms from the wake, the"
                 " address landed %lu ms after the release\n",
                 static_cast<unsigned long>(wifi.onlineMs()),
                 wifi.usedCachedAp() ? "cached" : "scanned",
                 wifi.usedLease() ? "installed" : "leased", wifi.ip(),
                 static_cast<unsigned long>((onlineUs - g_entryUs) / 1000),
                 static_cast<unsigned long>(addressAfterReleaseMs));

  // The one way the cache can fail without anything looking wrong: DHCP ran,
  // the address is fine, and the client never said how long it lives -- so
  // there is nothing to age an entry against and none is written. It would show
  // up only as every question paying for DHCP forever.
  if (!wifi.usedLease() && wifi.leaseSeconds() == 0) {
    Serial1.println("  the client did not say how long the lease lives -- nothing cached for the"
                    " next question");
  }

  const Backend::Result result = askRenewingStaleLease();
  const uint32_t roundTripMs = backend.elapsedMs();

  Serial1.printf("  round trip: %lu ms, first byte at %lu ms", static_cast<unsigned long>(roundTripMs),
                 static_cast<unsigned long>(backend.firstByteMs()));
  if (backend.status() != 0 && backend.firstByteMs() != 0) {
    Serial1.printf(" (%lu KB/s up)",
                   static_cast<unsigned long>(audio.wavBytes() / backend.firstByteMs()));
  }
  if (result == Backend::Result::Ok) {
    Serial1.printf(", %lu bytes of JSON\n", static_cast<unsigned long>(backend.bodyBytes()));
    Serial1.printf("  answer: \"%s\"\n", backend.answer());
  } else {
    Serial1.printf("\n  failed: %s\n", backend.lastError());
  }

  // The vision's error table, in the one place that owns it. Every outcome
  // chirps before it draws, answers included -- S7's note has why.
  Outcome outcome = Outcome::Broken;
  char detail[16] = {0};
  const char* title = nullptr;

  switch (result) {
    case Backend::Result::Ok:
      outcome = Outcome::Answered;
      break;
    case Backend::Result::NoServer:
      outcome = Outcome::NoServer;
      title = "NO SERVER";
      break;
    case Backend::Result::ServerError:
      outcome = Outcome::ServerError;
      title = "SERVER ERROR";
      snprintf(detail, sizeof(detail), "%d", backend.status());
      break;
    case Backend::Result::BadResponse:
      outcome = Outcome::BadResponse;
      title = "BAD RESPONSE";
      break;
    case Backend::Result::TimedOut:
      outcome = Outcome::TimedOut;
      title = "TIMED OUT";
      break;
  }

  // The chirp is timed out of the refresh rather than into it: it sounds first
  // on purpose -- the vision's sound section -- so the wait ends where it starts
  // and the refresh behind it is the panel's number and only the panel's.
  g_toChirpMs = sinceReleaseMs();
  if (title == nullptr) {
    stickyBuzzer::answer();
  } else {
    stickyBuzzer::error();
  }

  const int64_t refreshStartUs = esp_timer_get_time();
  if (title == nullptr) {
    screen.answer(backend.answer());
  } else {
    screen.error(title, detail[0] != '\0' ? detail : nullptr);
  }
  g_answerMs = static_cast<uint32_t>((esp_timer_get_time() - refreshStartUs) / 1000);
  Serial1.printf("  %s screen: %lu ms %s\n", title == nullptr ? "answer" : "error",
                 static_cast<unsigned long>(g_answerMs),
                 screen.lastWasPartial() ? "partial" : "full");

  logTiming(outcome, networkWaitMs, roundTripMs);
  finish(outcome);
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
