#pragma once

#include <Seeed_GFX.h>

// Shared with the host preview: crisp monochrome strokes, no bitmap asset.
namespace notebookIcon {
inline void draw(Seeed_GFX& gfx) {
  const int x = gfx.width() / 2 - 90;
  const int y = gfx.height() / 2 - 105;
  gfx.fillRect(x, y, 180, 220, TFT_BLACK);
  gfx.fillRect(x + 6, y + 6, 168, 208, TFT_WHITE);
  // Spine, four binding tabs, and ruled paper.
  gfx.fillRect(x + 28, y + 6, 4, 208, TFT_BLACK);
  for (int row = 0; row < 4; ++row) {
    gfx.fillRect(x - 12, y + 28 + row * 48, 30, 7, TFT_BLACK);
  }
  for (int row = 0; row < 5; ++row) {
    gfx.fillRect(x + 48, y + 43 + row * 30, row == 4 ? 65 : 104, 5, TFT_BLACK);
  }
}
}  // namespace notebookIcon
