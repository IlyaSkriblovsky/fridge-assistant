// Draws the screens and a glyph chart on the host. See README.md.
//
// The placement below is src/sticky/screen.cpp's, repeated rather than
// included, because screen.cpp reaches the panel through src/sticky/epaper.h
// and there is no panel here. Keep the two in step: a constant changed there
// and not here makes this lie.

#include <cstdio>
#include <cstring>

#include "Seeed_GFX.h"
#include "fonts/fonts.h"
#include "text.h"

namespace {

const TextFace& kWordFace = fontFreeSansBold24;
const TextFace& kAnswerFace = fontFreeSans24;
const TextFace& kDetailFace = fontFreeSans18;
constexpr uint8_t kWordSize = 2;
constexpr int32_t kAnswerMarginX = 40;
constexpr int32_t kWordBandMargin = 12;
constexpr int32_t kErrorBarHeight = 130;
constexpr int32_t kErrorDetailGap = 46;

void drawCentred(Seeed_GFX& gfx, const TextFace& face, const char* text,
                 int32_t centreX, int32_t middleY, uint8_t size) {
  const int32_t width = textWidth(face, text, size);
  const int32_t height = textBoxHeight(face, size);
  textDraw(gfx, face, text, centreX - width / 2, middleY - height / 2, size);
}

void save(const Seeed_GFX& gfx, const char* path) {
  FILE* file = fopen(path, "wb");
  fprintf(file, "P5\n%d %d\n255\n", gfx.width(), gfx.height());
  for (uint16_t pixel : gfx.px) {
    const unsigned char value = pixel ? 255 : 0;
    fwrite(&value, 1, 1, file);
  }
  fclose(file);
  printf("wrote %s\n", path);
}

// One panel's worth of screen, pasted into the tall sheet at `y`.
void paste(Seeed_GFX& sheet, const Seeed_GFX& panel, int32_t y) {
  for (int32_t row = 0; row < panel.height(); ++row) {
    for (int32_t col = 0; col < panel.width(); ++col) {
      sheet.px[(y + row) * sheet.width() + col] = panel.px[row * panel.width() + col];
    }
  }
}

// The four screens, with the word band drawn as two hairlines so that it can
// be seen to contain both words.
void screens(Seeed_GFX& sheet) {
  const int32_t band = textBoxHeight(kWordFace, kWordSize) + 2 * kWordBandMargin;
  const int32_t bandY = (480 - band) / 2;

  for (int screen = 0; screen < 4; ++screen) {
    Seeed_GFX panel(800, 480);
    panel.fillScreen(TFT_WHITE);
    panel.setTextColor(TFT_BLACK);
    panel.setTextSize(1);

    switch (screen) {
      case 0:
      case 1:
        panel.fillRect(0, bandY, 800, 1, TFT_BLACK);
        panel.fillRect(0, bandY + band - 1, 800, 1, TFT_BLACK);
        drawCentred(panel, kWordFace, screen == 0 ? "LISTENING" : "WORKING",
                    400, 240, kWordSize);
        break;
      case 2:
        textDraw(panel, kAnswerFace, "Молоко стоит до четверга", kAnswerMarginX,
                 240 - textBoxHeight(kAnswerFace, 1) / 2, 1);
        break;
      default: {
        const int32_t barY = (480 - kErrorBarHeight) / 2;
        panel.fillRect(0, barY, 800, kErrorBarHeight, TFT_BLACK);
        panel.setTextColor(TFT_WHITE);
        drawCentred(panel, kWordFace, "NO SERVER", 400, barY + kErrorBarHeight / 2, 1);
        panel.setTextColor(TFT_BLACK);
        const char* detail = "HTTP 503";
        textDraw(panel, kDetailFace, detail,
                 (800 - textWidth(kDetailFace, detail, 1)) / 2,
                 barY + kErrorBarHeight + kErrorDetailGap, 1);
        break;
      }
    }
    paste(sheet, panel, screen * 480);
  }

  // The invariant working() depends on: both words inside the band it is the
  // only part of the panel to push.
  printf("word band: y %d height %d\n", (int)bandY, (int)band);
  for (const char* word : {"LISTENING", "WORKING", "ЁЖ", "ΆΫ"}) {
    Seeed_GFX panel(800, 480);
    panel.fillScreen(TFT_WHITE);
    panel.setTextColor(TFT_BLACK);
    drawCentred(panel, kWordFace, word, 400, 240, kWordSize);
    int top = 480, bottom = -1;
    for (int y = 0; y < 480; ++y) {
      for (int x = 0; x < 800; ++x) {
        if (!panel.px[y * 800 + x]) {
          if (y < top) top = y;
          if (y > bottom) bottom = y;
        }
      }
    }
    printf("  %-12s ink %3d..%3d  width %3d  inside: %s\n", word, top, bottom,
           (int)textWidth(kWordFace, word, kWordSize),
           (top >= bandY && bottom < bandY + band) ? "yes" : "NO");
  }
}

void glyphs() {
  const char* rows[] = {
      "ΑΒΓΔΕΖΗΘΙΚΛΜΝΞΟΠΡΣΤΥΦΧΨΩ",
      "αβγδεζηθικλμνξοπρστυφχψως",
      "ΆΈΉΊΌΎΏΪΫ  άέήίόύώ ϊϋ ΐΰ",
      "Το γάλα λήγει την Πέμπτη.",
      "",
      "АБВГДЕЁЖЗИЙКЛМНОПРСТУФ",
      "ХЦЧШЩЪЫЬЭЮЯ",
      "абвгдеёжзийклмнопрстуф",
      "хцчшщъыьэюя",
      "Молоко до четверга, №7.",
      "",
      "ABCDEFGHIJKLMNOPQRSTUVWXYZ",
      "abcdefghijklmnopqrstuvwxyz",
      "0123456789 !\"#$%&'()*+,-./",
      ":;<=>?@[\\]^_`{|}~",
      "«» „“” ‘’ – — • … ° №",
      "日本語 — not built, one ? each",
  };
  const int count = sizeof(rows) / sizeof(rows[0]);
  const int32_t lineHeight = kAnswerFace.yAdvance;

  Seeed_GFX sheet(900, count * lineHeight + 40);
  sheet.fillScreen(TFT_WHITE);
  sheet.setTextColor(TFT_BLACK);
  sheet.setTextSize(1);
  for (int i = 0; i < count; ++i) {
    textDraw(sheet, kAnswerFace, rows[i], 20, 20 + i * lineHeight, 1);
  }
  save(sheet, "glyphs.pgm");
}

// textCopy() has to cut between characters, whatever the buffer's size lands
// in the middle of.
void copying() {
  printf("textCopy of \"Ёж—№\" (2, 2, 3, 3 bytes):\n");
  for (size_t size = 1; size <= 12; ++size) {
    char buffer[32];
    textCopy(buffer, size, "Ёж—№");
    printf("  size %2zu -> %2zu bytes  \"%s\"\n", size, strlen(buffer), buffer);
  }
}

}  // namespace

int main() {
  Seeed_GFX sheet(800, 480 * 4);
  sheet.fillScreen(TFT_WHITE);
  screens(sheet);
  save(sheet, "screens.pgm");
  glyphs();
  copying();
  return 0;
}
