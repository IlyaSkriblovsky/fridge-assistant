#pragma once
#include "text.h"
#include "fonts/fonts.h"

// Shared by the panel and host checks; reserve the footer and top indicator.
namespace reminderLayout {
constexpr int kWidth = 640, kHeight = 352, kTop = 56;
constexpr int kIcon = 56, kGap = 24, kFooterTop = 432;
struct Layout {
  const TextFace* face;
  int lines, height, x, y;
};
inline Layout measure(const char* text) {
  const TextFace* faces[] = {&fontFreeSans24, &fontFreeSans18, &fontFreeSans12};
  for (const auto* face : faces) {
    const int lines = textWrapLines(*face, text, 1, kWidth);
    const int height = (lines - 1) * face->yAdvance + textBoxHeight(*face, 1);
    if (height <= kHeight || face == &fontFreeSans12) {
      const int width = lines == 1 ? textWidth(*face, text, 1) : kWidth;
      return {face, lines, height, (800 - width - kIcon - kGap) / 2,
              kTop + (kHeight - height) / 2};
    }
  }
  return {};
}
inline void draw(Seeed_GFX& gfx, const char* text) {
  const auto box = measure(text);
  const int cx = box.x + kIcon / 2;
  const int cy = kTop + kHeight / 2;
  gfx.fillCircle(cx, cy, 27, TFT_BLACK);
  gfx.fillCircle(cx, cy, 24, TFT_WHITE);
  gfx.fillRect(cx - 1, cy - 17, 3, 19, TFT_BLACK);
  gfx.fillRect(cx, cy - 1, 15, 3, TFT_BLACK);
  textDrawWrapped(gfx, *box.face, text, box.x + kIcon + kGap, box.y,
                  1, kWidth, box.lines);
  constexpr const char* hint = "нажмите любую кнопку";
  textDraw(gfx, fontFreeSans12, hint,
           (800 - textWidth(fontFreeSans12, hint, 1)) / 2, kFooterTop, 1);
}
}
