#pragma once

#include <Seeed_GFX.h>

// Shared geometry and drawing for the panel and host preview. Byte-aligned
// window; every animation frame clears exactly the same rectangle.
namespace listeningWave {
constexpr int kX = 288, kY = 128, kWidth = 224, kHeight = 136;
constexpr int kLabelY = 320;
constexpr uint8_t kFrames = 3;

inline void draw(Seeed_GFX& gfx, uint8_t frame) {
  constexpr uint8_t heights[kFrames][9] = {
      {24, 52, 88, 128, 72, 104, 48, 80, 24},
      {40, 80, 120, 64, 96, 56, 112, 48, 32},
      {24, 48, 110, 94, 128, 88, 64, 96, 40},
  };
  gfx.fillRect(kX, kY, kWidth, kHeight, TFT_WHITE);
  for (int bar = 0; bar < 9; ++bar) {
    const int h = heights[frame % kFrames][bar];
    const int x = kX + 8 + bar * 24;
    const int y = kY + (kHeight - h) / 2;
    // Sixteen-pixel rounded bars, using integer scanlines on the 1-bit panel.
    constexpr uint8_t inset[] = {5, 3, 2, 1, 1, 0};
    gfx.fillRect(x, y + 6, 16, h - 12, TFT_BLACK);
    for (int row = 0; row < 6; ++row) {
      gfx.fillRect(x + inset[row], y + row, 16 - 2 * inset[row], 1, TFT_BLACK);
      gfx.fillRect(x + inset[row], y + h - 1 - row, 16 - 2 * inset[row], 1, TFT_BLACK);
    }
  }
}
}  // namespace listeningWave
