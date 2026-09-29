#pragma once

#include <Seeed_GFX.h>

namespace thinkingDots {
// Centered at the same height as the listening wave. Byte-aligned partial
// window includes the largest dot in every frame.
constexpr int kX = 315, kY = 172, kWidth = 170, kHeight = 48;
constexpr uint8_t kFrames = 4;

inline void draw(Seeed_GFX& gfx, uint8_t frame) {
  gfx.fillRect(kX, kY, kWidth, kHeight, TFT_WHITE);
  for (int dot = 0; dot < 3; ++dot) {
    const bool big = (dot == frame % kFrames) || (dot == 1 && frame == 3);
    const int radius = big ? 20 : 11;
    const int cx = 400 + (dot-1) * 64;
    const int cy = 196;
    gfx.fillCircle(cx, cy, radius, TFT_BLACK);
  }
}
}  // namespace thinkingDots
