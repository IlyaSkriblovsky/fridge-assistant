#pragma once
#include <Seeed_GFX.h>

namespace staleIcon {
constexpr int kX = 240, kY = 8, kWidth = 32, kHeight = 32;
inline void draw(Seeed_GFX& gfx, bool stale) {
  gfx.fillRect(kX, kY, kWidth, kHeight, TFT_WHITE);
  if (!stale) return;
  gfx.drawRect(kX + 5, kY + 4, 22, 24, TFT_BLACK);
  gfx.fillRect(kX + 14, kY + 8, 4, 10, TFT_BLACK);
  gfx.fillRect(kX + 14, kY + 21, 4, 3, TFT_BLACK);
}
}
