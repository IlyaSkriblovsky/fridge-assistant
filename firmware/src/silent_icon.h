#pragma once

#include <Seeed_GFX.h>

// Shared with the host preview. The entire byte-aligned rectangle belongs to
// this indicator: no answer text or battery pixels may enter it.
namespace silentIcon {
constexpr int kX = 176, kY = 8, kWidth = 48, kHeight = 32;

inline void draw(Seeed_GFX& gfx, bool enabled) {
  gfx.fillRect(kX, kY, kWidth, kHeight, TFT_WHITE);
  if (!enabled) return;
  gfx.fillRect(kX + 9, kY + 12, 7, 9, TFT_BLACK);
  for (int column = 0; column < 9; ++column)
    gfx.fillRect(kX + 15 + column, kY + 12 - column, 1, 9 + 2 * column, TFT_BLACK);
  // White separation keeps the diagonal visible where it crosses the speaker.
  for (int step = 0; step < 25; ++step)
    gfx.fillRect(kX + 8 + step, kY + 3 + step, 5, 1, TFT_WHITE);
  for (int step = 0; step < 25; ++step)
    gfx.fillRect(kX + 9 + step, kY + 3 + step, 3, 1, TFT_BLACK);
}
}  // namespace silentIcon
