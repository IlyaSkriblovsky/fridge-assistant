#pragma once

#include <Seeed_GFX.h>

#include <stddef.h>
#include <stdint.h>

// The three screens the device draws, and the only place the panel is touched.
//
// The interface is deliberately three calls wide. D4 moves the display into a
// task of its own once the upload streams, and that split is cheap exactly as
// long as nothing outside this class calls the panel: the day it happens, these
// three become messages on a queue and everything else stays as it is.
//
// The panel is a 3.97" 800x480 monochrome e-paper on an SSD1677 controller,
// through the two corrections in src/sticky_epaper.h -- which is included by
// the implementation rather than by this header, so the rest of the firmware
// never has to know the glass is wrong twice.
//
// Four things about the drawing are decisions rather than accidents:
//
//  * **No setRotation().** Seeed_GFX2's Board_reTerminal_Sticky already puts
//    the panel the way the vision's screen section settles on -- buttons along
//    the bottom edge, on the right, under the right thumb. A rotation appearing
//    here later is a regression, not a fix.
//  * **A full refresh on every transition** -- D6. Each of the three changes
//    most of the screen, and a full refresh clears accumulated ghosting as a
//    side effect. It costs about two and a half seconds, which is why the
//    buzzer exists.
//  * **ASCII only** -- D1. Only LOAD_GLCD and LOAD_GFXFF are compiled into
//    Seeed_GFX2 and the FreeFonts cover 0x20-0x7E, so answers arrive
//    transliterated on the backend. Anything outside that range is drawn as
//    '?', one per character rather than one per byte: a Russian answer that
//    slipped through has to look like text that could not be shown, not like a
//    blank screen and not like garbage.
//  * **No word wrap** -- D2. A long answer runs off the right edge, which is
//    accepted while the backend returns a fixed phrase.
//
// Nothing here draws by itself. begin() only brings the panel up; whether a
// cold start wants a white frame before the first real screen is the
// orchestrator's call, and clear() is what it calls.
class StickyScreen {
 public:
  // The longest string any screen draws, sanitised copy included. Well past
  // what fits on a line at the sizes below -- at the answer's 24 pt that is
  // about 45 characters -- so the truncation only ever loses text that was
  // already off the right edge under D2.
  static constexpr size_t kMaxTextChars = 128;

  // Brings the panel up. False is the one display failure the orchestrator
  // cannot draw a message about; the vision's answer to it is to chirp, log and
  // sleep. Calling it again once it has succeeded does nothing.
  bool begin();
  bool ready() const { return _ready; }

  // A white panel, refreshed. This is the cold-start pre-clear: after a
  // power-on the controller's previous-image RAM has nothing to do with what is
  // physically on the glass, and the first full refresh can ghost because of
  // it. On a wake it is one to two seconds spent flushing a panel the Listening
  // screen overwrites anyway -- see S8 in docs/implementation.md.
  void clear();

  // Asleep -> Listening. One word, as large as the panel takes, because it is
  // read from wherever the user is talking rather than up close.
  void listening();

  // Listening -> answer. Drawn from the left margin so an answer too long to
  // fit loses its end rather than both ends.
  void answer(const char* text);

  // Listening -> error. The title is one of the vision's error table; detail is
  // whatever narrows it down -- a status code -- and may be null or empty.
  void error(const char* title, const char* detail = nullptr);

  // Human-readable reason begin() returned false, straight from the library.
  const char* lastError() const { return _lastError; }

 private:
  // Every screen starts from the same state: white, black text, size 1. Set
  // rather than assumed, so no screen can inherit the last one's font.
  void startFrame();

  Seeed_GFX _display;
  bool _ready = false;
  const char* _lastError = "";
};

// Copies `text` into `out` as something the FreeFonts can draw: printable ASCII
// as it is, one '?' per character outside 0x20-0x7E, and at most size-1
// characters. UTF-8 continuation bytes are skipped, so a multi-byte character
// costs one '?' and not two or three.
//
// Exposed because it is ordinary logic with no panel behind it, and the only
// part of this module that can be reasoned about away from the device.
void screenDrawableText(char* out, size_t size, const char* text);
