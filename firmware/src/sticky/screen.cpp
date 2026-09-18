#include "sticky/screen.h"

#include <font/GFXFF/FreeSans18pt7b.h>
#include <font/GFXFF/FreeSans24pt7b.h>
#include <font/GFXFF/FreeSansBold24pt7b.h>

#include <string.h>

#include "sticky/epaper.h"

namespace {

// Three faces, and the reason for each.
//
// The panel is 235 dpi, so 24 pt lands at about 6 mm of line height and 18 pt
// at about 4.5 mm -- the first is a headline at arm's length, the second is
// subordinate to it without being small.
//
//  * kTitleFont, doubled, is the one word of listening() and working(): 516 px
//    of "LISTENING" on an 800 px panel, which is the largest of the three by a
//    long way because it is the screen read from across a room. "WORKING" is
//    narrower and sits in the same band, which is the point of the band.
//  * kTitleFont at size 1 is an error title, white inside the bar. The widest
//    in the vision's table is NO MICROPHONE at 416 px, so every title clears
//    the bar's edges with room to spare.
//  * kAnswerFont is the answer, and kDetailFont whatever narrows an error down.
const GFXfont* const kTitleFont = &FreeSansBold24pt7b;
const GFXfont* const kAnswerFont = &FreeSans24pt7b;
const GFXfont* const kDetailFont = &FreeSans18pt7b;

// The answer starts here and runs off the right edge if it has to -- D2.
constexpr int32_t kAnswerMarginX = 40;

// Added above and below the face's own line height to make the band working()
// refreshes. It buys back the rounding in the datum arithmetic and a few pixels
// of slack, and every pixel of it is rows the controller has to be sent, so it
// is small on purpose.
constexpr int32_t kWordBandMargin = 12;

// The error bar: full width, centred vertically, with the detail under it. A
// band of black is what tells an error from an answer at a glance, before
// either has been read.
constexpr int32_t kErrorBarHeight = 130;
constexpr int32_t kErrorDetailGap = 46;

}  // namespace

void screenDrawableText(char* out, size_t size, const char* text) {
  if (out == nullptr || size == 0) return;

  size_t n = 0;
  if (text != nullptr) {
    for (const uint8_t* p = reinterpret_cast<const uint8_t*>(text); *p && n + 1 < size; ++p) {
      if (*p >= 0x20 && *p <= 0x7E) {
        out[n++] = static_cast<char>(*p);
      } else if ((*p & 0xC0) == 0x80) {
        // A UTF-8 continuation byte. Its character was marked by the lead byte
        // that came before it, so this one costs nothing.
        continue;
      } else {
        out[n++] = '?';
      }
    }
  }
  out[n] = '\0';
}

bool StickyScreen::begin() {
  if (_ready) return true;

  if (!_display.begin<Board_reTerminal_Sticky, Config_Sticky_SSD1677_Fixed>()) {
    _lastError = _display.lastResult().message;
    return false;
  }

  _ready = true;
  return true;
}

void StickyScreen::startFrame() {
  _display.fillScreen(TFT_WHITE);
  _display.setTextColor(TFT_BLACK);  // one argument: no background fill
  _display.setTextSize(1);
  _display.setTextDatum(TL_DATUM);
}

void StickyScreen::clear() {
  if (!_ready) return;

  _display.fillScreen(TFT_WHITE);
  _display.refresh();
  _drewFullFrame = true;
  _lastWasPartial = false;
}

void StickyScreen::refreshWhole() {
  // A full refresh has to have run this boot, or the shadow the partial is
  // differential against is the all-white one a fresh allocation starts as
  // while the glass still holds the last question's answer.
  if (_drewFullFrame && _display.refreshPartial(0, 0, _display.width(), _display.height()).ok()) {
    _lastWasPartial = true;
    return;
  }

  if (_drewFullFrame) _lastError = _display.lastResult().message;
  _display.refresh();
  _drewFullFrame = true;
  _lastWasPartial = false;
}

void StickyScreen::startWord() {
  _display.setTextColor(TFT_BLACK);
  _display.setFreeFont(kTitleFont);
  _display.setTextSize(2);
  _display.setTextDatum(MC_DATUM);
}

void StickyScreen::wordBand(int32_t& y, int32_t& height) {
  height = _display.fontHeight() + 2 * kWordBandMargin;
  y = (_display.height() - height) / 2;
}

void StickyScreen::listening() {
  if (!_ready) return;

  startFrame();
  startWord();
  _display.drawString("LISTENING", _display.width() / 2, _display.height() / 2);
  _display.refresh();
  _drewFullFrame = true;
  _lastWasPartial = false;
}

bool StickyScreen::working() {
  if (!_ready) return false;

  startWord();

  int32_t y = 0;
  int32_t height = 0;
  wordBand(y, height);

  // The frame buffer outside the band is left exactly as listening() drew it,
  // because it is exactly what is on the glass and the partial refresh will not
  // send it. Only the band is repainted, and only the band is pushed.
  _display.fillRect(0, y, _display.width(), height, TFT_WHITE);
  _display.drawString("WORKING", _display.width() / 2, _display.height() / 2);

  const GfxResult result = _display.refreshPartial(0, y, _display.width(), height);
  _lastWasPartial = result.ok();
  if (!result.ok()) {
    _lastError = result.message;
    return false;
  }
  return true;
}

void StickyScreen::answer(const char* text) {
  if (!_ready) return;

  char drawable[kMaxTextChars];
  screenDrawableText(drawable, sizeof(drawable), text);

  startFrame();
  _display.setFreeFont(kAnswerFont);
  _display.setTextDatum(ML_DATUM);
  _display.drawString(drawable, kAnswerMarginX, _display.height() / 2);
  refreshWhole();
}

void StickyScreen::error(const char* title, const char* detail) {
  if (!_ready) return;

  char drawableTitle[kMaxTextChars];
  char drawableDetail[kMaxTextChars];
  screenDrawableText(drawableTitle, sizeof(drawableTitle), title);
  screenDrawableText(drawableDetail, sizeof(drawableDetail), detail);

  const int32_t barY = (_display.height() - kErrorBarHeight) / 2;

  startFrame();
  _display.fillRect(0, barY, _display.width(), kErrorBarHeight, TFT_BLACK);

  _display.setFreeFont(kTitleFont);
  _display.setTextColor(TFT_WHITE);
  _display.setTextDatum(MC_DATUM);
  _display.drawString(drawableTitle, _display.width() / 2, barY + kErrorBarHeight / 2);

  if (drawableDetail[0] != '\0') {
    _display.setFreeFont(kDetailFont);
    _display.setTextColor(TFT_BLACK);
    _display.setTextDatum(TC_DATUM);
    _display.drawString(drawableDetail, _display.width() / 2,
                        barY + kErrorBarHeight + kErrorDetailGap);
  }

  refreshWhole();
}
