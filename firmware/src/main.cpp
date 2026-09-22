// The orchestrator: wake, latch, microphone, capture task, ready chirp, WiFi and
// the Listening screen, the request opened and the recording streamed into it
// while the button is held, release, the tail of the body, taken chirp, answer
// or error chirp, draw, answer dwell, dashboard, deep sleep.
//
// **The order at the front is load-bearing and measured.** Capture starts
// before the chirp, because the chirp means "the microphone is live" and a
// chirp that came first would put its own duration into the dead time at the
// front of every question -- E1 and E3, and the vision's step 3. Nothing may be
// inserted between mic.begin() and stickyBuzzer::ready() that can block.
//
// **The panel is posted to, never waited on**, except at the exit. A screen is
// 0.8 to 2.3 s of the panel's own timeline against 300 ms of network and
// 500 ms of round trip (S5, S7b, E7, E8), so Display draws on a task of its
// own: this thread sees the release when it happens and asks the backend while
// the panel catches up. The one wait is past the last chirp, where deep sleep
// would otherwise cut a refresh in half.
//
// **The upload goes up under the hold.** The request opens once the press can
// no longer turn out to be a tap and the network is up, whichever is later, and
// every pass of the loop that watches the recording sends what the capture task
// has committed since the last one. What is left after the release is the last
// few chunks of audio, the terminating chunk and the backend's answer. A
// request that fails while the user is still talking ends the question there
// and then, for the reason the vision gives a WiFi drop: a recording with
// nowhere to go is abandoned rather than finished.
//
// **Nothing prints between the release and the last chirp** unless the
// question has already failed. Serial1 at 115200 is a millisecond for every
// eleven characters and blocks once the UART's FIFO is full; the lines that
// describe the wait are written once it is over.
//
// finish() returns a voice cycle to the idle controller. Only runIdle() (or
// an offline Up wake) enters deep sleep, after the display finishes.
//
// The stale-lease rule, openRenewingStaleLease(), lives here because it spans
// Backend and WifiLink and neither half can see it alone.

#include <Arduino.h>
#include <esp_timer.h>
#include <esp_private/esp_clk.h>
#include <esp_attr.h>
#include <soc/rtc.h>
#include "config.h"
#include "dashboard.h"
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
#include "silent_mode.h"
#include "wifi_link.h"

namespace {

constexpr int kPinLogRx = 44;
constexpr int kPinLogTx = 43;

// How often the association and the capture task are looked at while the button
// is held. Small, because it is also the resolution of WifiLink::elapsedMs(),
// how late this thread can be to a release, and what a chunk of the body
// carries: whatever the capture task committed in one pass, a 16 ms read or two.
constexpr uint32_t kPollMs = 10;

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
  Idle,    // startup without a press
  Tap,     // too short to be a question
};

StickyButton button;
StickyMic mic;
Recording audio;
Capture capture;
WifiLink wifi;
Display display;
Backend backend(secrets::kBackendBaseUrl, secrets::kDeviceToken);
Dashboard dashboard;
RTC_DATA_ATTR uint64_t dashboardDeadlineTicks = 0;
enum class IdlePhase { Sleep, AfterVoice, Fetch };
IdlePhase idlePhase = IdlePhase::Fetch;
Outcome lastOutcome = Outcome::Idle;
bool resumeDashboard = false;
bool recordingLogged = false;
bool coldScreen = true;
bool aiArmed = true;

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
// apart so the log can say what the wait was made of. The tail is everything it
// took to get the rest of the body up -- the last chunks and the terminating
// one, or on a question whose request could not open before the release, the
// connect and the whole body. The answer is the time spent inside receive().
uint32_t g_tailMs = 0;
uint32_t g_takenChirpMs = 0;
uint32_t g_networkWaitMs = 0;
uint32_t g_answerWaitMs = 0;

// Display::start(), which brings the panel up on this thread rather than on the
// task -- display.h has why.
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
    case Outcome::Idle: return "idle";
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
  if (recordingLogged || capture.stopReason() == Capture::StopReason::None) return;
  recordingLogged = true;

  Serial1.printf("  recording: %s after %lu ms held -- %lu ms of audio, %lu bytes to send\n",
                 capture.stopReasonName(), static_cast<unsigned long>(button.heldMs()),
                 static_cast<unsigned long>(audio.recordedMs()),
                 static_cast<unsigned long>(audio.wavBytes()));

  // The drop detector. The task's own clock beyond the audio it kept is audio
  // the DMA threw away, and should be a chunk or two. Slow reads come one in
  // fifteen from the DMA's own block size; more than that is something else
  // competing for the core. Capture's accessors have the rest.
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
// queue -- the one thing the display task cannot take out of a question (E8).
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
  if (which == Display::Screen::Idle || which == Display::Screen::Listening || which == Display::Screen::Answer ||
      which == Display::Screen::Error) {
    if (shown.batteryPercent >= 0) Serial1.printf(", battery %d%%", shown.batteryPercent);
    else Serial1.print(", battery unavailable");
    if (shown.climate.valid)
      Serial1.printf(", temperature %.1f C, humidity %.1f%%",
                     shown.climate.temperatureC, shown.climate.humidityPercent);
    else Serial1.print(", climate unavailable");
  }
  if (shown.error[0] != '\0') Serial1.printf(" -- the partial was refused: %s", shown.error);
  Serial1.println();
}

// What the question cost the user, which is the release to the last chirp --
// the refresh after it is time the panel is readable through, not time spent
// waiting. Printed as its parts because each of them belongs to a different
// decision: the confirmation to the debounce and the poll, the tail to the
// streaming upload, the network to the lease cache, the answer to the backend
// and E2.
//
// **The parts are the chain and not the calendar.** Everything here is time
// this thread spent in one thing after the release, in the order it spent it,
// so they add up to the total with whatever is left named as such. The address
// arriving is deliberately not one of them: it happens in the WiFi task and
// usually lands during the recording, so counting it from the release would
// count the same milliseconds twice. What is counted is the wait that was left
// when this thread got there, which is nothing at all on a wake with a lease.
//
// The second line is the answer on the glass: the screens of one question
// queue on one controller, and the answer waits for whatever is ahead of it
// there.
void logTiming(Outcome outcome, bool panelIdle) {
  if (g_lastChirpUs == 0 || g_releaseUs == 0) return;

  const bool answered = outcome == Outcome::Answered;
  const uint32_t toChirpMs = millisBetween(g_releaseUs, g_lastChirpUs);
  const uint32_t confirmMs = millisBetween(g_releaseUs, g_releaseSeenUs);
  const uint32_t partsMs =
      confirmMs + g_tailMs + g_takenChirpMs + g_networkWaitMs + g_answerWaitMs;

  Serial1.printf("  release to the %s chirp: %lu ms -- %lu ms for the release to be confirmed,"
                 " %lu ms of tail, %lu ms taken chirp, %lu ms waiting for the network, %lu ms"
                 " waiting for the answer, %lu ms else\n",
                 answered ? "answer" : "error", static_cast<unsigned long>(toChirpMs),
                 static_cast<unsigned long>(confirmMs), static_cast<unsigned long>(g_tailMs),
                 static_cast<unsigned long>(g_takenChirpMs),
                 static_cast<unsigned long>(g_networkWaitMs),
                 static_cast<unsigned long>(g_answerWaitMs),
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

void waitForRelease() {
  stickyPower::waitForWakeButtonsReleased();
}

// End a voice cycle without sleeping: loop() owns the dwell and dashboard.
void finish(Outcome outcome) {
  capture.abort();
  capture.wait(kCaptureJoinMs);
  mic.end();
  backend.close();
  logRecording();
  lastOutcome = outcome;
  idlePhase = outcome == Outcome::Tap || outcome == Outcome::Idle
      ? (resumeDashboard ? IdlePhase::Fetch : IdlePhase::Sleep)
      : IdlePhase::AfterVoice;
  // A capped/failed question may leave AI held. Only a fresh press starts one.
  aiArmed = !button.isDown();
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
void fail(Outcome outcome, const char* title, const char* detail) {
  Serial1.printf("  %s: %s\n", title, detail != nullptr ? detail : "");

  if (startDisplay()) {
    display.error(title, detail);
  } else {
    Serial1.printf("  and nothing to show it on: %s\n", display.lastError());
  }
  stickyBuzzer::error();

  finish(outcome);
}

// The request, opened with the stale-lease rule around it. False means it
// could not be, and backend.result() says how.
//
// **A connect that nothing answered is the only thing that can say a cached
// lease has gone stale**, because a lease that has outlived its network installs
// exactly as well as a good one and fails only when a packet needs to go
// somewhere. So it is read that way -- but not on the first one. A healthy
// network on this desk produces a connect that fails outright about once in
// fifteen questions (S7, and E7 could not find why), and a lease dropped on one
// of those costs 3.2 s on a wake where nothing was wrong. Connecting twice costs
// one connect timeout on a wake that was already going to be slow, which is the
// cheaper of the two mistakes.
//
// Everything else the backend can do -- refuse the connection, answer 500,
// answer nothing, answer nonsense -- proves there is something at the address
// and therefore that the address works, and leaves the lease alone. Only the
// connect can fail this way, so only the open is retried: a connection that
// answered and then died is not the lease.
//
// This usually runs while the button is held, and the recording goes on
// underneath it: a retry costs the user nothing until the release, and every
// retry is a new request that starts again from the header.
bool openRenewingStaleLease() {
  if (backend.open()) return true;

  const bool couldBeStale = backend.unreachable() && wifi.usedLease();
  if (!couldBeStale) return false;

  Serial1.printf("  nothing answered in %lu ms, and this question is on a reused address"
                 " -- connecting once more before believing it\n",
                 static_cast<unsigned long>(millisBetween(backend.openUs(), backend.doneUs())));

  const bool opened = backend.open();
  if (opened || !backend.unreachable()) {
    Serial1.println("  the second connect got somewhere -- the lease was not the problem");
    return opened;
  }

  Serial1.println("  twice, so the address is the suspect -- dropping the lease and asking DHCP");
  const uint32_t stale = wifi.ipv4();
  wifi.renewAddress();
  while (wifi.poll() == WifiLink::State::Connecting) delay(kPollMs);

  if (!wifi.online()) {
    Serial1.printf("  no address after %lu ms: %s\n", static_cast<unsigned long>(wifi.renewMs()),
                   wifi.lastError());
    return false;
  }

  // A server usually hands back the address it handed out before, and when it
  // does the lease was never the problem: the question is about to fail again
  // for whatever reason it failed the first time, and the 3.2 s just spent was
  // the price of a false positive. Worth saying out loud, because it is the one
  // line in the log that tells the two apart.
  Serial1.printf("  DHCP took %lu ms and handed back %s -- %s\n",
                 static_cast<unsigned long>(wifi.renewMs()), wifi.ip(),
                 static_cast<uint32_t>(wifi.ipv4()) == stale
                     ? "the same address, so the lease was not what was wrong"
                     : "a different address, so it was");

  return backend.open();
}

// Sends whatever the capture task has committed that has not gone up yet. The
// offset is the request's own byte count, so a request that was opened again
// starts from the header; and the count is published by the task with the
// ordering that makes everything below it safe to read (Recording).
bool sendCommitted() {
  const size_t ready = audio.wavBytes();
  const size_t sent = backend.sentBytes();
  return ready == sent || backend.write(audio.wav() + sent, ready - sent);
}

// The rest of the body and the terminating chunk, once the recording is over.
bool endBody() { return sendCommitted() && backend.end(); }

// The request's own story, for the log: when it opened against the hold, what
// it carried, where its longest write was, and the two halves the round trip
// splits into -- the tail after the release, which is what streaming exists to
// shrink, and the answer after the terminating chunk. The second is
// read after the taken chirp, so a backend quicker than the chirp reads as the
// chirp: Backend::firstByteUs() has why.
void logStream(Backend::Result result) {
  if (backend.openUs() == 0) return;

  Serial1.printf("  request: opened %lu ms into setup()",
                 static_cast<unsigned long>(millisBetween(g_entryUs, backend.openUs())));
  if (backend.connectedUs() == 0) {
    Serial1.println(", and never connected");
  } else {
    const uint32_t connectMs = millisBetween(backend.openUs(), backend.connectedUs());
    const uint32_t longestAtMs = millisBetween(g_entryUs, backend.longestWriteAtUs());
    Serial1.printf(", connected in %lu ms; %lu bytes of the recording's %lu in %lu chunks, the"
                   " longest write %lu ms at %lu ms into setup()\n",
                   static_cast<unsigned long>(connectMs),
                   static_cast<unsigned long>(backend.sentBytes()),
                   static_cast<unsigned long>(audio.wavBytes()),
                   static_cast<unsigned long>(backend.frames()),
                   static_cast<unsigned long>(backend.longestWriteUs() / 1000),
                   static_cast<unsigned long>(longestAtMs));
  }

  if (backend.endUs() != 0 && g_releaseUs != 0) {
    Serial1.printf("  the terminating chunk %lu ms after the release",
                   static_cast<unsigned long>(millisBetween(g_releaseUs, backend.endUs())));
    if (backend.firstByteUs() != 0) {
      const uint32_t firstByteMs = millisBetween(backend.endUs(), backend.firstByteUs());
      const uint32_t doneMs = millisBetween(backend.endUs(), backend.doneUs());
      Serial1.printf(", the answer's first byte read %lu ms after that, all of it %lu ms after"
                     " that",
                     static_cast<unsigned long>(firstByteMs), static_cast<unsigned long>(doneMs));
    }
    Serial1.println();
  }

  if (result == Backend::Result::Ok) {
    Serial1.printf("  answer: \"%s\", %lu bytes of JSON\n", backend.answer(),
                   static_cast<unsigned long>(backend.replyBytes()));
  } else {
    Serial1.printf("  failed: %s\n", backend.lastError());
  }
}

// The end of every question that reached for the backend: the vision's error
// table in the one place that owns it, then the screen, the chirp, the lines
// the wait could not afford, and sleep. Every outcome chirps before its screen
// appears, answers included -- S7's note has why.
//
// Also reached from under the hold, when the request fails while the user is
// still talking -- which is why the recording is stopped first. It is a
// no-op on one that has already stopped.
void conclude(Backend::Result result) {
  if (result == Backend::Result::NoServer && !wifi.online())
    return fail(Outcome::NoWifi, "NO WIFI", nullptr);
  capture.abort();

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
  // 230 ms of blocking, so the panel starts on the answer while it sounds, and
  // the answer still appears a second after the chirp. The wait ends where the
  // chirp starts.
  g_lastChirpUs = esp_timer_get_time();
  if (title == nullptr) {
    display.answer(backend.answer());
    stickyBuzzer::answer();
  } else {
    display.error(title, detail[0] != '\0' ? detail : nullptr);
    stickyBuzzer::error();
  }

  // The lines the wait above could not afford.
  capture.wait(kCaptureJoinMs);
  logRecording();

  // Timed by the WiFi task rather than by this one, because this one is only
  // ever told afterwards -- see WifiLink::onlineMs(). It is the radio's answer
  // to "when was there a network", and it is not the same question as the
  // network wait: an address that lands during the recording costs the
  // question nothing.
  const int64_t onlineUs = g_beginUs + static_cast<int64_t>(wifi.onlineMs()) * 1000;
  Serial1.printf("  online in %lu ms (%s AP, %s address), %s, DNS %s (%s) -- %lu ms from the wake",
                 static_cast<unsigned long>(wifi.onlineMs()),
                 wifi.usedCachedAp() ? "cached" : "scanned",
                 wifi.usedLease() ? "installed" : "leased", wifi.ip(), wifi.dns(),
                 wifi.usedCustomDns() ? "secrets.h" : "the network's",
                 static_cast<unsigned long>(millisBetween(g_entryUs, onlineUs)));
  if (g_releaseUs != 0) {
    Serial1.printf(", the address landed %lu ms after the release",
                   static_cast<unsigned long>(millisBetween(g_releaseUs, onlineUs)));
  }
  Serial1.println();

  // The one way the cache can fail without anything looking wrong: DHCP ran,
  // the address is fine, and the client never said how long it lives -- so
  // there is nothing to age an entry against and none is written. It would show
  // up only as every question paying for DHCP forever.
  if (!wifi.usedLease() && wifi.leaseSeconds() == 0) {
    Serial1.println("  the client did not say how long the lease lives -- nothing cached for the"
                    " next question");
  }

  logStream(result);
  finish(outcome);
}

// One pass of the upload under the hold: the request opened the first time
// through, then whatever the capture task has committed since the last pass. A
// failure here ends the question now, with the user still talking.
bool stream() {
  if ((!backend.streaming() && !openRenewingStaleLease()) || !sendCommitted()) {
    conclude(backend.result());
    return false;
  }
  return true;
}

}  // namespace

void runVoice(int64_t pressUs) {
  g_entryUs = pressUs;
  g_releaseUs = g_releaseSeenUs = g_lastChirpUs = 0;
  g_tailMs = g_takenChirpMs = g_networkWaitMs = g_answerWaitMs = 0;
  recordingLogged = false;
  button.begin(g_entryUs);
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

  // Capture before the chirp, and nothing that can block between them -- see
  // the top of this file.
  if (!mic.begin()) return fail(Outcome::Broken, "NO MICROPHONE", mic.lastError());
  if (!audio.begin(mic.sampleRate())) return fail(Outcome::Broken, "NO MEMORY", audio.lastError());
  if (!capture.start(mic, audio, button)) {
    return fail(Outcome::Broken, "NO MICROPHONE", "the capture task would not start");
  }
  // Timed as the chirp starts, not as it returns: the chirp is 110 ms of
  // blocking, and a timestamp taken after it would report the end of the chirp
  // as the moment the microphone went live. Measured from the top of setup(),
  // so the boot is not in it; S9 has the boot.
  const int64_t chirpUs = esp_timer_get_time();
  stickyBuzzer::ready();
  Serial1.printf("  ready chirp %lu ms into setup(), with the recording already running\n",
                 static_cast<unsigned long>((chirpUs - g_entryUs) / 1000));

  if (!wifi.online()) {
    g_beginUs = esp_timer_get_time();
    if (!wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword, secrets::kDnsServer))
      return fail(Outcome::NoWifi, "NO WIFI", wifi.lastError());
  }

  // The one failure with nothing to draw a message on: chirp, log and sleep.
  if (!startDisplay()) {
    Serial1.printf("  the panel would not start: %s\n", display.lastError());
    stickyBuzzer::error();
    return finish(Outcome::Broken);
  }

  // A cold start is the only time the controller's previous-image RAM has
  // nothing to do with what is on the glass. Posted ahead of LISTENING and never
  // superseded by it -- see Display.
  if (coldScreen) { display.clear(); coldScreen = false; }
  display.listening();

  // Everything from here until the release is the capture task's; this loop
  // watches it, and feeds the upload what it has committed. A drop is the
  // vision's rule that a recording with nowhere to go is aborted rather than
  // finished.
  //
  // **The request waits for the press to stop being a tap**, which the capture
  // task says rather than the clock: a release in the last debounce window is
  // not known yet, and a tap has to reach nothing. On a wake with a lease the
  // network is up at about 300 ms (S7b), which is where the minimum hold ends
  // anyway, so on most questions the two arrive together.
  while (!capture.finished()) {
    if (wifi.poll() == WifiLink::State::Failed) {
      capture.abort();
    } else if (wifi.online() && capture.pastMinimumHold()) {
      if (!stream()) return;
    }
    delay(kPollMs);
  }
  g_releaseSeenUs = esp_timer_get_time();

  mic.end();

  g_releaseUs = g_entryUs + static_cast<int64_t>(button.heldMs()) * 1000;

  if (capture.stopReason() == Capture::StopReason::ReadFailed) {
    logRecording();
    return fail(Outcome::Broken, "NO MICROPHONE", capture.lastError());
  }

  // Silent on purpose -- D7. The chirp below is the first sound a question
  // makes after the ready chirp, and a tap must not make it. Nothing was opened
  // for it either: the loop above waited for the minimum hold.
  if (button.isTap()) {
    logRecording();
    Serial1.println("  too short to be a question -- nothing sent");
    display.idle();
    return finish(Outcome::Tap);
  }

  // The loop above aborts for one reason only: the association failed, before
  // the address arrived or after.
  if (capture.stopReason() == Capture::StopReason::Aborted) {
    logRecording();
    Serial1.printf("  no network after %lu ms: %s\n", static_cast<unsigned long>(wifi.elapsedMs()),
                   wifi.lastError());
    return fail(Outcome::NoWifi, "NO WIFI", nullptr);
  }

  // The vision's step 6, in two parts: the chirp says the question was taken,
  // and the word says the same thing to somebody who was not listening. The
  // word waits its turn on the panel -- behind the rest of LISTENING on a short
  // hold -- and is dropped unseen if the answer overtakes it there.
  display.working();

  // The tail, on a question whose request opened under the hold: the chunks
  // committed since the loop's last pass and the terminating chunk. It goes
  // before the chirp rather than after it -- the chirp is 60 ms of blocking,
  // and the backend can spend them thinking instead of waiting for the end of
  // the body.
  const bool streamed = backend.streaming();
  if (streamed) {
    const int64_t startUs = esp_timer_get_time();
    const bool ended = endBody();
    g_tailMs = millisBetween(startUs, esp_timer_get_time());
    if (!ended) return conclude(backend.result());
  }

  {
    const int64_t startUs = esp_timer_get_time();
    stickyBuzzer::taken();
    g_takenChirpMs = millisBetween(startUs, esp_timer_get_time());
  }

  // A release that arrives before the address does is not a failure: the
  // association has a budget of its own and NO WIFI is what happens when that
  // runs out. The whole body then goes up after the release, which is also why
  // the chirp above did not wait for it.
  //
  // Timed from here rather than from the release on purpose: this is the poll
  // loop's own view and the question it answers is the poller's -- how much of
  // the wait was left when this thread got here. The radio's view of the same
  // moment is wifi.onlineMs(), in the log.
  if (!streamed) {
    {
      const int64_t startUs = esp_timer_get_time();
      while (wifi.poll() == WifiLink::State::Connecting) delay(kPollMs);
      g_networkWaitMs = millisBetween(startUs, esp_timer_get_time());
    }

    if (!wifi.online()) {
      logRecording();
      Serial1.printf("  no network after %lu ms: %s\n",
                     static_cast<unsigned long>(wifi.elapsedMs()), wifi.lastError());
      return fail(Outcome::NoWifi, "NO WIFI", nullptr);
    }

    const int64_t startUs = esp_timer_get_time();
    const bool ended = openRenewingStaleLease() && endBody();
    g_tailMs = millisBetween(startUs, esp_timer_get_time());
    if (!ended) return conclude(backend.result());
  }

  const int64_t startUs = esp_timer_get_time();
  const Backend::Result result = backend.receive();
  g_answerWaitMs = millisBetween(startUs, esp_timer_get_time());

  conclude(result);
}

namespace {

void scheduleDashboard(uint64_t receivedTicks, uint32_t seconds) {
  dashboardDeadlineTicks = receivedTicks + rtc_time_us_to_slowclk(
      seconds * 1000000ULL, esp_clk_slowclk_cal_get());
}

uint64_t nextSleepUs() {
  const uint64_t now = rtc_time_get();
  if (!dashboardDeadlineTicks) scheduleDashboard(now, config::kDashboardDefaultSeconds);
  // Store raw ticks: recalibration at boot must not rescale an absolute uptime
  // and move the deadline. Only the remaining duration uses current calibration.
  const uint64_t left = dashboardDeadlineTicks > now
      ? rtc_time_slowclk_to_us(dashboardDeadlineTicks - now, esp_clk_slowclk_cal_get()) : 0;
  return dashboardProtocol::sleepUs(left, 0);
}

// Poll on every idle path, including panel and sensor waits. Never join a
// cancelled network worker before starting capture.
bool idleButton() {
  dashboard.poll();
  static int64_t releasedUs = 0;
  if (!button.isDown()) {
    if (!releasedUs) releasedUs = esp_timer_get_time();
    if (esp_timer_get_time() - releasedUs >= config::kButtonDebounceMs * 1000LL)
      aiArmed = true;
  } else {
    releasedUs = 0;
    if (aiArmed) {
      dashboard.cancel();
      resumeDashboard = idlePhase != IdlePhase::Sleep;
      return true;
    }
  }
  static int64_t upSinceUs = 0;
  static bool upHandled = false;
  if (digitalRead(stickyPower::kPinUpButton) != LOW) {
    upSinceUs = 0;
    upHandled = false;
  } else if (!upHandled) {
    if (!upSinceUs) upSinceUs = esp_timer_get_time();
    if (esp_timer_get_time() - upSinceUs >= config::kButtonDebounceMs * 1000LL &&
        display.waitIdle(0)) {
      upHandled = true;
      if (silentMode::toggle() && startDisplay()) display.silentIndicator();
    }
  }
  return false;
}

// False means a new voice press; true means the operation is complete.
bool waitPanel() {
  for (;;) {
    if (idleButton()) return false;
    if (display.waitIdle(0)) return true;
    delay(kPollMs);
  }
}

void dashboardFailed() {
  Serial1.println("  dashboard unavailable; keeping the previous screen, retry in one hour");
  scheduleDashboard(rtc_time_get(), config::kDashboardDefaultSeconds);
  if (startDisplay()) {
    if (coldScreen) {
      display.clear();
      display.error("NO DASHBOARD", nullptr);
      coldScreen = false;
    } else display.staleIndicator();
  }
  idlePhase = IdlePhase::Sleep;
}

// Runs until sleep or a new AI press. All long panel/network operations live
// on their own tasks, so this loop can hand a press straight to runVoice().
void runIdle() {
  if (idlePhase == IdlePhase::AfterVoice) {
    if (!waitPanel()) return;
    const auto which = lastOutcome == Outcome::Answered ? Display::Screen::Answer : Display::Screen::Error;
    const int64_t drawnUs = display.record(which).endUs;
    logScreen("final voice screen", which);
    logTiming(lastOutcome, true);
    // With no working panel there is no completed refresh to wait from.
    const int64_t until = (drawnUs ? drawnUs : esp_timer_get_time()) + config::kAnswerDwellMs * 1000LL;
    while (esp_timer_get_time() < until) {
      if (idleButton()) return;
      delay(kPollMs);
    }
    idlePhase = IdlePhase::Fetch;
  }
  if (idlePhase == IdlePhase::Fetch) {
    if (idleButton()) return;
    if (!startDisplay()) { dashboardFailed(); }
    else {
      if (!waitPanel()) return;
      display.sensors();
      if (!waitPanel()) return;
      const auto snapshot = display.record(Display::Screen::Sensors);
      if (!wifi.online()) g_beginUs = esp_timer_get_time();
      if (!wifi.online() && !wifi.begin(secrets::kWifiSsid, secrets::kWifiPassword, secrets::kDnsServer)) {
        dashboardFailed();
      } else {
        while (wifi.poll() == WifiLink::State::Connecting) {
          if (idleButton()) return;
          delay(kPollMs);
        }
        // A previous request may still be returning from cancellation.
        while (!dashboard.done()) {
          if (idleButton()) return;
          delay(kPollMs);
        }
        if (!wifi.online() || !dashboard.start(snapshot.batteryPercent, snapshot.climate)) {
          dashboardFailed();
        } else {
          while (!dashboard.done()) {
            if (idleButton()) return;
            delay(kPollMs);
          }
          if (idleButton()) return;
          if (!dashboard.ok()) dashboardFailed();
          else {
            scheduleDashboard(dashboard.receivedRtcTicks(), dashboard.nextSeconds());
            Serial1.printf("  dashboard: 48000 bytes, next update in %lu s\n",
                           static_cast<unsigned long>(dashboard.nextSeconds()));
            if (coldScreen) { display.clear(); coldScreen = false; }
            display.dashboard(dashboard.pixels());
            idlePhase = IdlePhase::Sleep;
          }
        }
      }
    }
  }
  if (!waitPanel()) return;
  while (!dashboard.done()) {
    if (idleButton()) return;
    delay(kPollMs);
  }
  // Preserve the deadline even if Up (or a held AI after a capped recording)
  // delays entry to sleep. AI remains responsive while Up is held.
  while (digitalRead(stickyPower::kPinUpButton) == LOW || button.isDown()) {
    if (idleButton()) return;
    delay(kPollMs);
  }
  if (idleButton()) return;
  if (!waitPanel()) return;
  logScreen("dashboard", Display::Screen::Dashboard);
  logScreen("dashboard status", Display::Screen::Stale);
  Serial1.printf("  %s; panel startup %lu ms, display stack unused %lu bytes\n",
                 outcomeName(lastOutcome), static_cast<unsigned long>(g_panelUpMs),
                 static_cast<unsigned long>(display.stackUnusedBytes()));
  Serial1.printf("  sleeping; dashboard timer in %llu ms\n",
                 static_cast<unsigned long long>(nextSleepUs() / 1000));
  dashboard.close();
  wifi.end();
  Serial1.flush();
  stickyPower::deepSleep(nextSleepUs());
}

}  // namespace

void setup() {
  g_entryUs = esp_timer_get_time();
  stickyPower::holdLatch();
  stickyPower::enableUpWake();
  button.begin(g_entryUs);
  Serial1.begin(115200, SERIAL_8N1, kPinLogRx, kPinLogTx);
  coldScreen = !stickyPower::wokeFromDeepSleep();
  if (coldScreen) dashboardDeadlineTicks = 0;
  const bool preferenceLoaded = silentMode::load();
  if (!preferenceLoaded) Serial1.println("  sound preference unreadable; muted");
  const bool upWake = stickyPower::wokeFromDeepSleep() &&
      esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT1 &&
      (esp_sleep_get_ext1_wakeup_status() & (1ULL << stickyPower::kPinUpButton));
  if (upWake) {
    if (!preferenceLoaded || !silentMode::toggle())
      Serial1.println("  silent mode could not be saved; unchanged");
    Serial1.printf("  silent mode: %s\n", silentMode::enabled() ? "on" : "off");
    if (startDisplay()) {
      display.silentIndicator();
      display.waitIdle(UINT32_MAX);
      logScreen("silent indicator", Display::Screen::Silent);
    }
    waitForRelease();
    Serial1.flush();
    stickyPower::deepSleep(nextSleepUs());
  }
  Serial1.printf("wake %s, reset %s\n", stickyPower::wakeupCauseName(), stickyPower::resetReasonName());
  // Keep this physical check in setup: exp_e1_firmware wraps it for its rig.
  if (button.isDown()) runVoice(g_entryUs);
  else idlePhase = coldScreen || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER
      ? IdlePhase::Fetch : IdlePhase::Sleep;
}

void loop() {
  runIdle();
  runVoice(esp_timer_get_time());
}
