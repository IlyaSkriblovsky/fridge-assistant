// Host stub: just enough of Seeed_GFX2 to run src/text.cpp off the device.
// drawChar() below is drawCharGfx() from Seeed_GFX.cpp, transcribed.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <vector>

#define PROGMEM
#define pgm_read_byte(a) (*(const uint8_t*)(a))
#define pgm_read_word(a) (*(const uint16_t*)(a))
#define pgm_read_ptr(a)  (*(void* const*)(a))

#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF

typedef struct { uint32_t bitmapOffset; uint8_t width, height, xAdvance; int8_t xOffset, yOffset; } GFXglyph;
typedef struct { uint8_t* bitmap; GFXglyph* glyph; uint16_t first, last; uint8_t yAdvance; } GFXfont;

class Seeed_GFX {
 public:
  Seeed_GFX(int w, int h) : _w(w), _h(h), px(w * h, 0xFFFF) {}
  int16_t width() const { return _w; }
  int16_t height() const { return _h; }
  void fillScreen(uint32_t c) { for (auto& p : px) p = c; }
  void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t c) {
    for (int32_t j = y; j < y + h; ++j)
      for (int32_t i = x; i < x + w; ++i)
        if (i >= 0 && i < _w && j >= 0 && j < _h) px[j * _w + i] = c;
  }
  void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t c) { fillRect(x, y, w, 1, c); }
  void setTextColor(uint16_t c) { textcolor = textbgcolor = c; }
  void setTextSize(uint8_t s) { textsize = s ? s : 1; }
  void setFreeFont(const GFXfont* f) { gfxFont = (GFXfont*)f; }

  int16_t drawChar(uint16_t uniCode, int32_t x, int32_t y, uint8_t /*font*/) {
    if (!gfxFont || uniCode < gfxFont->first || uniCode > gfxFont->last) return 0;
    const GFXglyph* g = &gfxFont->glyph[uniCode - gfxFont->first];
    const uint8_t* bitmap = gfxFont->bitmap;
    uint32_t bo = g->bitmapOffset;
    uint8_t w = g->width, h = g->height;
    int8_t xo = g->xOffset, yo = g->yOffset;
    uint8_t size = textsize;
    int16_t xo16 = size > 1 ? xo : 0, yo16 = size > 1 ? yo : 0;
    uint8_t bits = 0, bit = 0;
    uint16_t hpc = 0;
    for (uint8_t yy = 0; yy < h; yy++) {
      uint8_t xx = 0;
      for (xx = 0; xx < w; xx++) {
        if (bit == 0) { bits = pgm_read_byte(&bitmap[bo++]); bit = 0x80; }
        if (bits & bit) hpc++;
        else if (hpc) {
          if (size == 1) drawFastHLine(x + xo + xx - hpc, y + yo + yy, hpc, textcolor);
          else fillRect(x + (xo16 + xx - hpc) * size, y + (yo16 + yy) * size, size * hpc, size, textcolor);
          hpc = 0;
        }
        bit >>= 1;
      }
      if (hpc) {
        if (size == 1) drawFastHLine(x + xo + xx - hpc, y + yo + yy, hpc, textcolor);
        else fillRect(x + (xo16 + xx - hpc) * size, y + (yo16 + yy) * size, size * hpc, size, textcolor);
        hpc = 0;
      }
    }
    return g->xAdvance * size;
  }

  int _w, _h;
  std::vector<uint16_t> px;
  uint16_t textcolor = 0, textbgcolor = 0;
  uint8_t textsize = 1;
  GFXfont* gfxFont = nullptr;
};
