#pragma once

#include <Seeed_GFX.h>

#include <stddef.h>
#include <stdint.h>

// The four screens the device draws, and the only place the panel is touched.
//
// The interface is deliberately four calls wide. D4 moves the display into a
// task of its own once the upload streams, and that split is cheap exactly as
// long as nothing outside this class calls the panel: the day it happens, these
// four become messages on a queue and everything else stays as it is.
//
// The panel is a 3.97" 800x480 monochrome e-paper on an SSD1677 controller,
// through the two corrections in src/sticky/epaper.h -- which is included by
// the implementation rather than by this header, so the rest of the firmware
// never has to know the glass is wrong twice.
//
// Four things about the drawing are decisions rather than accidents:
//
//  * **No setRotation().** Seeed_GFX2's Board_reTerminal_Sticky already puts
//    the panel the way the vision's screen section settles on -- buttons along
//    the bottom edge, on the right, under the right thumb. A rotation appearing
//    here later is a regression, not a fix.
//  * **One full refresh per question, and it is the first one** -- D6.
//    listening() is full and everything after it is partial, which is not a
//    preference but the only arrangement that works. A partial update is
//    differential: the controller picks each pixel's waveform from the pair
//    (what is on the glass, what should be), and "what is on the glass" is the
//    shadow in src/sticky/epaper.h -- allocated fresh on every boot and seeded
//    white. After a deep sleep the glass still holds the last answer and the
//    firmware has been told nothing about it, so a partial listening() would
//    leave that answer's black pixels exactly where they are and draw the word
//    on top. Only a full refresh drives every pixel whatever it was, which is
//    what reconciles the glass with the buffer once per wake -- and it also
//    clears accumulated ghosting, which is the other thing D6 is about.
//
//    So the cost is paid where it is not felt. The Listening refresh runs under
//    the recording; the three that follow it are partial and each is a little
//    over a third of the price. clear() is the cold-start form of the same
//    reconciliation and is full for the same reason.
//  * **ASCII only** -- D1. Only LOAD_GLCD and LOAD_GFXFF are compiled into
//    Seeed_GFX2 and the FreeFonts cover 0x20-0x7E, so answers arrive
//    transliterated on the backend. Anything outside that range is drawn as
//    '?', one per character rather than one per byte: a Russian answer that
//    slipped through has to look like text that could not be shown, not like a
//    blank screen and not like garbage.
//  * **No word wrap** -- D2. A long answer runs off the right edge, which is
//    accepted while the backend returns a fixed phrase.
//
// **listening() and working() are one screen with two words in it**, which is
// what makes the second a partial refresh rather than a fourth full one. Both
// draw a single word in the same face at the same size, centred, so the band of
// panel the word occupies is a property of the face and not of the word --
// wordBand() is that band, and working() repaints it and refreshes it alone.
// Everything outside the band is identical between the two screens and is never
// sent to the controller a second time.
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

  // Listening -> working. The vision's step 6: the button is up, the question
  // is on its way, and the panel would otherwise still read LISTENING until the
  // answer lands. The word alone is repainted, through a partial refresh --
  // this is the one transition the user waits through, so a full refresh here
  // would put its two and a half seconds in front of a round trip that is
  // usually half a second (E7).
  //
  // **It has to follow a full refresh in the same boot**, which listening()
  // always is: a partial update is differential against what the controller was
  // last told is on the glass, and after a wake the only thing that has told it
  // anything is that refresh.
  //
  // False when the controller refused the partial update, which leaves
  // LISTENING on the glass and lastError() with the reason. It is not worth
  // failing a question over -- the answer's own full refresh repaints
  // everything either way -- so the caller logs it and carries on.
  bool working();

  // Working -> answer. Drawn from the left margin so an answer too long to fit
  // loses its end rather than both ends. Partial, over the whole panel.
  void answer(const char* text);

  // Working -> error. The title is one of the vision's error table; detail is
  // whatever narrows it down -- a status code -- and may be null or empty.
  // Partial, over the whole panel, with one exception: an error drawn before
  // listening() ever ran has no full refresh behind it and takes one.
  void error(const char* title, const char* detail = nullptr);

  // Whether the last screen drawn got its partial refresh. False means it fell
  // back to a full one, which is not a failure the user sees -- it costs
  // seconds, not correctness -- but is worth a line in the log.
  bool lastWasPartial() const { return _lastWasPartial; }

  // Human-readable reason begin() or working() returned false, straight from
  // the library.
  const char* lastError() const { return _lastError; }

 private:
  // Every screen starts from the same state: white, black text, size 1. Set
  // rather than assumed, so no screen can inherit the last one's font.
  void startFrame();

  // The one word of listening() and working(): the face, the size and the
  // centring the two share. Called by both, so neither can drift out of the
  // band the other refreshes.
  void startWord();

  // The band that word occupies, full panel width. The height comes from the
  // face rather than from a constant: drawString centres a FreeFont's glyphs
  // within its ascent plus descent, while fontHeight() is the line advance,
  // which is larger -- so a band of fontHeight() clears the word top and bottom
  // without measuring a single glyph. startWord() must have run first.
  void wordBand(int32_t& y, int32_t& height);

  // The whole panel, refreshed differentially against the shadow. Falls back to
  // a full refresh when the controller refuses, because a screen that does not
  // appear is a worse failure than a slow one -- and because the fallback is
  // also the right answer for the one caller that has no full refresh behind
  // it. Only safe after a full refresh has run this boot; see the note above.
  void refreshWhole();

  Seeed_GFX _display;
  bool _ready = false;
  bool _drewFullFrame = false;  // a full refresh has run this boot
  bool _lastWasPartial = false;
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
