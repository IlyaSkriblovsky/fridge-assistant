#pragma once

#include <Seeed_GFX.h>

#include <stddef.h>
#include <stdint.h>

#include "text.h"

// The four screens the device draws, and the only place the panel is touched.
//
// Its one caller is Display, in src/display.h, which owns the only instance and
// draws it on a task of its own. Everything below runs on that task and blocks
// it for the length of a refresh, which is what the task is for.
//
// The panel is a 3.97" 800x480 monochrome e-paper on an SSD1677 controller,
// through the two corrections in src/sticky/epaper.h -- which is included by
// the implementation rather than by this header, so the rest of the firmware
// never has to know the glass is wrong twice.
//
// Four things about the drawing are decisions rather than accidents:
//
//  * **Landscape, buttons along the top edge.** setRotation(2) turns the
//    board profile's default image through 180 degrees, as preferred in use.
//  * **One full refresh per question, and it is the first one** -- the
//    vision's screen section. listening() is full and everything after it is
//    partial, which is not a preference but the only arrangement that works. A
//    partial update is differential: the controller picks each pixel's
//    waveform from the pair (what is on the glass, what should be), and "what
//    is on the glass" is the shadow in src/sticky/epaper.h -- allocated fresh
//    on every boot and seeded white. After a deep sleep the glass still holds
//    the last answer and the firmware has been told nothing about it, so a
//    partial listening() would leave that answer's black pixels exactly where
//    they are and draw the word on top. Only a full refresh drives every pixel
//    whatever it was, which is what reconciles the glass with the buffer once
//    per wake -- and it also clears accumulated ghosting, which is the other
//    thing full refreshes are for.
//
//    So the cost is paid where it is not felt. The Listening refresh runs under
//    the recording; the three that follow it are partial and each is a little
//    over a third of the price. clear() is the cold-start form of the same
//    reconciliation and is full for the same reason.
//  * **Latin, Greek and Cyrillic, and nothing else drawn as itself.** The
//    faces are in src/fonts/, laid out by src/text.h, which is also where the
//    reasons for both live. Nothing here measures or centres through
//    Seeed_GFX2: its textWidth() reads bytes rather than characters, so it
//    measures every non-Latin string as empty, and drawString() centres on
//    that. Anything outside the faces is drawn as '?', one per character
//    rather than one per byte: a word in a script we never built has to look
//    like text that could not be shown, not like a blank screen and not like
//    garbage.
//  * **The answer wraps, into a box with a margin on all four sides.** Lines
//    are broken by src/text.h and the block of them is centred vertically, so
//    a one-line answer sits where a one-line answer always sat and a longer
//    one grows around that. An answer with more in it than the box has lines
//    for ends in an ellipsis rather than losing its tail quietly; showing the
//    rest is pagination, which is where D2 goes next. The error screen does
//    not wrap: its title comes from the vision's error table and its detail is
//    a status code, and neither has ever been longer than the panel is wide.
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
  // The longest string any screen draws, in bytes, and the longest detail
  // under an error title.
  //
  // The answer is what sets the first: it wraps now, so the bound is a whole
  // panel of text rather than a line of it. Measured on the host in
  // tools/preview, the panel holds seven lines of 24 pt, and filling all seven
  // takes 375 bytes of a real Russian sentence or 560 of the narrowest Cyrillic
  // letter with no spaces to break on. So 640 is past anything the panel can
  // show, and what textCopy() drops is text textDrawWrapped() would have
  // dropped as well -- with the difference that running out of lines leaves an
  // ellipsis to say so. The one string that beats it is a panel of the
  // narrowest three-byte punctuation, 1260 bytes, and a sentence made of
  // quotation marks is not an answer.
  //
  // The detail is an HTTP status code and has never been anything else, so it
  // is sized as the short string it is: both buffers are copied onto a task's
  // stack in Display::post().
  static constexpr size_t kMaxTextChars = 640;
  static constexpr size_t kMaxDetailChars = 64;

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
  // answer lands. The word alone is repainted, through a partial refresh: the
  // answer queues behind it on the one controller, so a full refresh here
  // would put its two and a half seconds in front of the answer's own. It has
  // to follow a full refresh in the same boot, which listening() is -- see the
  // note on partial updates above.
  //
  // False when the controller refused the partial update, which leaves
  // LISTENING on the glass and lastError() with the reason. It is not worth
  // failing a question over -- the answer's own full refresh repaints
  // everything either way -- so the caller logs it and carries on.
  bool working();

  // Working -> answer. Wrapped into the answer box and centred in it as a
  // block, left-aligned: a ragged right edge reads as text, and a centred one
  // reads as a poster. Partial, over the whole panel.
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
  // rather than assumed, so no screen can inherit the last one's colour.
  void startFrame();

  // Draws `text` with its box centred on the point, which is the placement
  // MC_DATUM used to give. Done here rather than through a datum because
  // Seeed_GFX2 would measure the string with the textWidth() described above.
  void drawCentred(const TextFace& face, const char* text, int32_t centreX,
                   int32_t middleY, uint8_t size);

  // The band the one word of listening() and working() occupies, full panel
  // width. Both draw in the same face at the same size and both centre on the
  // same point, so the band is the face's box plus a margin and neither word
  // can leave it.
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
