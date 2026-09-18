// S8 -- The flow, with S10's display task under it. The orchestrator: wake,
// latch, microphone, capture task, ready chirp, WiFi and the Listening screen,
// release, taken chirp, the working screen, upload, answer or error chirp,
// draw, deep sleep.
//
// **The order at the front is load-bearing and measured.** Capture starts
// before the chirp, because the chirp means "the microphone is live" and a
// chirp that came first would put its own duration into the dead time at the
// front of every question -- E1 and E3, and the vision's step 3. Nothing may be
// inserted between mic.begin() and stickyBuzzer::ready() that can block.
//
// **The panel is posted to, never waited on**, except at the exit. A screen is
// 0.8 to 2.3 s of the panel's own timeline against 300 ms of network and
// 500 ms of round trip (S5, S7b, E7, E8), and until S10 this thread sat inside
// every one of them: a release during LISTENING waited out the rest of it, and
// WORKING stood between the release and the upload on every question. Display
// draws on a task of its own now, so this thread sees the release when it
// happens and asks the backend while the panel catches up. The one wait left
// is past the last chirp, where deep sleep would otherwise cut a refresh in
// half. Two things about the screens are unchanged from S8, and both follow
// from what the panel costs:
//
//  * The pre-clear is a cold-start thing. A full white frame before the first
//    real screen is what a controller whose previous-image RAM has nothing to
//    do with the glass needs; on a wake it is 2373 ms spent flushing a panel
//    the Listening screen overwrites anyway.
//  * The working screen is a partial refresh -- S8 settled the vision's step 6
//    that way. It is no longer in the wait at all, but it is still the screen
//    the user reads while waiting, and a full refresh would put the answer
//    2.4 s behind it on the panel's own queue.
//
// **Nothing prints between the release and the last chirp** unless the
// question has already failed. Serial1 at 115200 is a millisecond for every
// eleven characters and blocks once the UART's FIFO is full, and that wait is
// the number S10 is about; the lines that describe it are written once it is
// over.
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

#include "backend.h"
#include "capture.h"
#include "display.h"
#include "recording.h"
#include "wifi_link.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// How often the association and the capture task are looked at while the button
// is held. Small, because it is also the resolution of WifiLink::elapsedMs() --
// and, since S10, how late this thread can be to a release.
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

// How long finish() gives the panel to go idle before it sleeps anyway.
// Bounded for the capture join's reason: the library waits up to 30 s on a BUSY
// pin that never drops, and a question must not end by hanging with the rail
// latched. The longest queue a question can leave is a cold start's -- the
// pre-clear, LISTENING and the answer, about 2.3 + 2.3 + 1.3 s (S8, S9) -- so
// this is that with room to spare, and a panel still busy past it is wedged.
constexpr uint32_t kDisplayDrainMs = 10000;

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
Display display;
Backend backend;

// The moments the question is measured against, and two of them are not the
// moment this thread reaches them:
//
//  * The release is the top of setup() plus the hold -- the moment the line
//    went high, as the capture task timed it. This thread hears about it a
//    debounce window and up to a poll later, and that gap is part of the wait.
//  * Online is wifi.begin() plus WifiLink::onlineMs(), which is the WiFi task's
//    own timestamp of the address arriving.
//
// The panel's moments are not here at all: they are Display's records, taken on
// its own task, because a timestamp taken here around a post would time the
// post. All of these are zero until they happen.
int64_t g_entryUs = 0;
int64_t g_releaseUs = 0;
int64_t g_releaseSeenUs = 0;
int64_t g_beginUs = 0;
int64_t g_lastChirpUs = 0;

// What this thread spent after the release, in the order it spent it. Kept
// apart so the log can say what the wait was made of.
uint32_t g_takenChirpMs = 0;
uint32_t g_networkWaitMs = 0;
uint32_t g_roundTripMs = 0;

// Display::start(), which brings the panel up on this thread. S10 settled that
// it stays here rather than on the task, and this is the number that says so.
uint32_t g_panelUpMs = 0;

uint32_t millisBetween(int64_t fromUs, int64_t toUs) {
  return toUs > fromUs ? static_cast<uint32_t>((toUs - fromUs) / 1000) : 0;
}

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

// The recording, as the capture task saw it. Printed at most once, and never
// while a question is waiting on this thread: after the last chirp, or on a
// path that has already failed.
void logRecording() {
  static bool logged = false;
  if (logged || capture.stopReason() == Capture::StopReason::None) return;
  logged = true;

  Serial1.printf("  recording: %s after %lu ms held -- %lu ms of audio, %lu bytes to send\n",
                 capture.stopReasonName(), static_cast<unsigned long>(button.heldMs()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(audio.wavBytes()));

  // The drop detector, and the baseline S10 is checked against. The task's own
  // clock beyond the audio it kept is audio the DMA threw away, and should be a
  // chunk or two. Slow reads come one in fifteen from the DMA's own block size;
  // more than that is something else competing for the core. Capture's
  // accessors have the rest.
  Serial1.printf("  capture task: %lu ms reading for %lu ms kept, %lu chunks, %lu slow, the"
                 " longest %lu us at chunk %lu\n",
                 static_cast<unsigned long>(capture.elapsedMs()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(capture.chunks()),
                 static_cast<unsigned long>(capture.slowChunks()),
                 static_cast<unsigned long>(capture.longestChunkUs()),
                 static_cast<unsigned long>(capture.longestAtChunk()));
}

// One screen, off Display's record of it, on the same axis as the hold: the
// top of setup(). A screen that waited says how long, which is the panel's own
// queue -- the one thing S10 cannot take out of a question (E8).
void logScreen(const char* name, Display::Screen which) {
  const Display::Record& shown = display.record(which);
  if (shown.postedUs == 0) return;

  if (shown.superseded) {
    Serial1.printf("  %s: superseded while it waited for the panel -- never drawn\n", name);
    return;
  }
  if (shown.startUs == 0) return;

  Serial1.printf("  %s: %lu ms %s, %lu to %lu ms into setup()", name,
                 static_cast<unsigned long>(millisBetween(shown.startUs, shown.endUs)),
                 shown.partial ? "partial" : "full",
                 static_cast<unsigned long>(millisBetween(g_entryUs, shown.startUs)),
                 static_cast<unsigned long>(millisBetween(g_entryUs, shown.endUs)));
  const uint32_t queuedMs = millisBetween(shown.postedUs, shown.startUs);
  if (queuedMs != 0) {
    Serial1.printf(", %lu ms queued behind the panel", static_cast<unsigned long>(queuedMs));
  }
  if (shown.error[0] != '\0') Serial1.printf(" -- the partial was refused: %s", shown.error);
  Serial1.println();
}

// What the question cost the user, which is the release to the last chirp --
// the refresh after it is time the panel is readable through, not time spent
// waiting. Printed as its parts because each of them belongs to a different
// decision: the confirmation to the debounce and the poll, the network to S7b,
// the round trip to E7 and D4.
//
// **The parts are the chain and not the calendar.** Everything here is time
// this thread spent in one thing after the release, in the order it spent it,
// so they add up to the total with whatever is left named as such. The address
// arriving is deliberately not one of them: it happens in the WiFi task and
// usually lands during the recording, so counting it from the release would
// count the same milliseconds twice. What is counted is the wait that was left
// when this thread got there, which is nothing at all on a wake with a lease.
//
// The second line is the answer on the glass, which S10 shortens less: the
// screens of one question still queue on one controller, and the answer waits
// for whatever is ahead of it there.
void logTiming(Outcome outcome, bool panelIdle) {
  if (g_lastChirpUs == 0 || g_releaseUs == 0) return;

  const bool answered = outcome == Outcome::Answered;
  const uint32_t toChirpMs = millisBetween(g_releaseUs, g_lastChirpUs);
  const uint32_t confirmMs = millisBetween(g_releaseUs, g_releaseSeenUs);
  const uint32_t partsMs = confirmMs + g_takenChirpMs + g_networkWaitMs + g_roundTripMs;

  Serial1.printf("  release to the %s chirp: %lu ms -- %lu ms for the release to be confirmed,"
                 " %lu ms taken chirp, %lu ms waiting for the network, %lu ms round trip, %lu ms"
                 " else\n",
                 answered ? "answer" : "error", static_cast<unsigned long>(toChirpMs),
                 static_cast<unsigned long>(confirmMs),
                 static_cast<unsigned long>(g_takenChirpMs),
                 static_cast<unsigned long>(g_networkWaitMs),
                 static_cast<unsigned long>(g_roundTripMs),
                 static_cast<unsigned long>(toChirpMs > partsMs ? toChirpMs - partsMs : 0));

  const Display::Record& shown =
      display.record(answered ? Display::Screen::Answer : Display::Screen::Error);
  if (!panelIdle || shown.endUs == 0) return;

  Serial1.printf("  release to the %s on the glass: %lu ms -- %lu ms queued behind the panel,"
                 " %lu ms of %s refresh\n",
                 answered ? "answer" : "error",
                 static_cast<unsigned long>(millisBetween(g_releaseUs, shown.endUs)),
                 static_cast<unsigned long>(millisBetween(shown.postedUs, shown.startUs)),
                 static_cast<unsigned long>(millisBetween(shown.startUs, shown.endUs)),
                 shown.partial ? "partial" : "full");
}

// A press held through the log is still down when the sleep is armed, and ext1
// wakes on the level.
void waitForRelease() {
  if (!button.isDown()) return;

  const int64_t deadlineUs = esp_timer_get_time() + kReleaseWaitMs * 1000LL;
  Serial1.println("  waiting for the button to come back up");
  while (button.isDown() && esp_timer_get_time() < deadlineUs) delay(5);
}

// The one exit. Called straight after whichever screen was posted, so
// everything above it has already happened -- except, usually, the panel.
[[noreturn]] void finish(Outcome outcome) {
  // The capture task owns the microphone until it is joined, so the abort comes
  // before mic.end() and not after it. Both are no-ops when the task never
  // started or has already reported.
  capture.abort();
  capture.wait(kCaptureJoinMs);
  mic.end();
  wifi.end();

  logRecording();

  // Nothing sleeps with the panel mid-refresh: deep sleep parks EPD_EN and the
  // rail goes with it. This is past the last chirp, so it is awake time rather
  // than wait, and it is printed apart from the wait for that reason.
  const int64_t drainStartUs = esp_timer_get_time();
  const bool panelIdle = display.waitIdle(kDisplayDrainMs);
  const uint32_t drainMs = millisBetween(drainStartUs, esp_timer_get_time());

  if (panelIdle) {
    logScreen("pre-clear", Display::Screen::Clear);
    logScreen("LISTENING", Display::Screen::Listening);
    logScreen("WORKING", Display::Screen::Working);
    logScreen("answer screen", Display::Screen::Answer);
    logScreen("error screen", Display::Screen::Error);
  } else {
    Serial1.printf("  the panel had not finished after %lu ms -- sleeping anyway\n",
                   static_cast<unsigned long>(drainMs));
  }
  logTiming(outcome, panelIdle);

  if (display.running()) {
    Serial1.printf("  panel: up in %lu ms on this thread, %lu ms waited for before sleeping;"
                   " %lu of %lu bytes of the display task's stack never used\n",
                   static_cast<unsigned long>(g_panelUpMs), static_cast<unsigned long>(drainMs),
                   static_cast<unsigned long>(display.stackUnusedBytes()),
                   static_cast<unsigned long>(Display::kStackBytes));
  }

  Serial1.printf("  %s, %lu ms awake\n", outcomeName(outcome),
                 static_cast<unsigned long>((esp_timer_get_time() - g_entryUs) / 1000));

  waitForRelease();
  Serial1.flush();

  stickyPower::deepSleep();
}

// Display::start(), timed. Every path to a screen goes through it, and only the
// first call does anything.
bool startDisplay() {
  if (display.running()) return true;

  const int64_t startUs = esp_timer_get_time();
  const bool started = display.start();
  g_panelUpMs = millisBetween(startUs, esp_timer_get_time());
  return started;
}

// A failure: log it, show it, chirp, sleep. The screen is posted before the
// chirp rather than after it for the reason every screen is: the post costs
// nothing, and the chirp is 450 ms the panel would otherwise spend waiting for
// this thread to finish making a sound.
[[noreturn]] void fail(Outcome outcome, const char* title, const char* detail) {
  Serial1.printf("  %s: %s\n", title, detail != nullptr ? detail : "");

  if (startDisplay()) {
    display.error(title, detail);
  } else {
    Serial1.printf("  and nothing to show it on: %s\n", display.lastError());
  }
  stickyBuzzer::error();

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
  if (!startDisplay()) {
    Serial1.printf("  the panel would not start: %s\n", display.lastError());
    stickyBuzzer::error();
    finish(Outcome::Broken);
  }

  // A cold start is the only time the controller's previous-image RAM has
  // nothing to do with what is on the glass. Posted ahead of LISTENING and never
  // superseded by it -- see Display.
  if (!stickyPower::wokeFromDeepSleep()) display.clear();
  display.listening();

  // Everything from here until the release is the capture task's; this loop
  // only watches. A drop is the vision's rule that a recording with nowhere to
  // go is aborted rather than finished.
  while (!capture.finished()) {
    if (wifi.poll() == WifiLink::State::Failed) capture.abort();
    delay(kPollMs);
  }
  g_releaseSeenUs = esp_timer_get_time();

  mic.end();

  g_releaseUs = g_entryUs + static_cast<int64_t>(button.heldMs()) * 1000;

  if (capture.stopReason() == Capture::StopReason::ReadFailed) {
    logRecording();
    fail(Outcome::Broken, "NO MICROPHONE", capture.lastError());
  }

  // Silent on purpose -- D7. The chirp below is the first sound a question
  // makes after the ready chirp, and a tap must not make it.
  if (button.isTap()) {
    logRecording();
    Serial1.println("  too short to be a question -- nothing sent");
    finish(Outcome::Tap);
  }

  // The vision's step 6, in two parts: the chirp says the question was taken,
  // and the word says the same thing to somebody who was not listening. The
  // word waits its turn on the panel -- behind the rest of LISTENING on a short
  // hold -- and is dropped unseen if the answer overtakes it there.
  display.working();
  {
    const int64_t startUs = esp_timer_get_time();
    stickyBuzzer::taken();
    g_takenChirpMs = millisBetween(startUs, esp_timer_get_time());
  }

  // A release that arrives before the address does is not a failure: the
  // association has a budget of its own and NO WIFI is what happens when that
  // runs out.
  //
  // Timed from here rather than from the release on purpose: this is the poll
  // loop's own view and the question it answers is the poller's -- how much of
  // the wait was left when this thread got here. The radio's view of the same
  // moment is wifi.onlineMs(), below.
  {
    const int64_t startUs = esp_timer_get_time();
    while (wifi.poll() == WifiLink::State::Connecting) delay(kPollMs);
    g_networkWaitMs = millisBetween(startUs, esp_timer_get_time());
  }

  if (!wifi.online()) {
    logRecording();
    Serial1.printf("  no network after %lu ms: %s\n", static_cast<unsigned long>(wifi.elapsedMs()),
                   wifi.lastError());
    fail(Outcome::NoWifi, "NO WIFI", nullptr);
  }

  const Backend::Result result = askRenewingStaleLease();
  g_roundTripMs = backend.elapsedMs();

  // The vision's error table, in the one place that owns it. Every outcome
  // chirps before its screen appears, answers included -- S7's note has why.
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

  // Posted first and chirped second: the post costs nothing and the chirp is
  // 230 ms of blocking, so the panel starts on the answer while it sounds -- and
  // the answer still appears a second after the chirp, which is what S7 moved
  // the chirp in front of the refresh for. The wait ends where the chirp starts.
  g_lastChirpUs = esp_timer_get_time();
  if (title == nullptr) {
    display.answer(backend.answer());
    stickyBuzzer::answer();
  } else {
    display.error(title, detail[0] != '\0' ? detail : nullptr);
    stickyBuzzer::error();
  }

  // The lines the wait above could not afford.
  logRecording();

  // Timed by the WiFi task rather than by this one, because this one is only
  // ever told afterwards -- see WifiLink::onlineMs(). It is the radio's answer
  // to "when was there a network", and it is not the same question as the
  // network wait: an address that lands during the recording costs the
  // question nothing.
  const int64_t onlineUs = g_beginUs + static_cast<int64_t>(wifi.onlineMs()) * 1000;
  Serial1.printf("  online in %lu ms (%s AP, %s address), %s -- %lu ms from the wake, the"
                 " address landed %lu ms after the release\n",
                 static_cast<unsigned long>(wifi.onlineMs()),
                 wifi.usedCachedAp() ? "cached" : "scanned",
                 wifi.usedLease() ? "installed" : "leased", wifi.ip(),
                 static_cast<unsigned long>(millisBetween(g_entryUs, onlineUs)),
                 static_cast<unsigned long>(millisBetween(g_releaseUs, onlineUs)));

  // The one way the cache can fail without anything looking wrong: DHCP ran,
  // the address is fine, and the client never said how long it lives -- so
  // there is nothing to age an entry against and none is written. It would show
  // up only as every question paying for DHCP forever.
  if (!wifi.usedLease() && wifi.leaseSeconds() == 0) {
    Serial1.println("  the client did not say how long the lease lives -- nothing cached for the"
                    " next question");
  }

  Serial1.printf("  round trip: %lu ms, first byte at %lu ms",
                 static_cast<unsigned long>(g_roundTripMs),
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

  finish(outcome);
}

void loop() {
  // Never reached: setup() always ends in deep sleep.
}
