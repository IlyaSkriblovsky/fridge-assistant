#include "text.h"

#include <string.h>

namespace {

// Drawn in place of anything the face has no glyph for. One per character,
// not one per byte: a word in a script we never built stays readable as text
// that could not be shown.
constexpr uint32_t kFallback = '?';

// What textNextCodepoint() returns for bytes that are not UTF-8. It is in no
// face, so it resolves to kFallback like any other unknown character.
constexpr uint32_t kReplacement = 0xFFFD;

// A hole in a group's range -- a code point the range spans but the face does
// not draw. tools/gfxfont.py writes these with no bitmap and no advance,
// which is what tells them from a space: a space has no bitmap either, but it
// does advance.
bool isHole(const GFXglyph* glyph) {
  return pgm_read_byte(&glyph->xAdvance) == 0 &&
         pgm_read_byte(&glyph->width) == 0 &&
         pgm_read_byte(&glyph->height) == 0;
}

// The glyph for a code point, and the font it came from, or null. A hole
// keeps the search going rather than ending it: the generated ranges do not
// overlap today, and this is what makes that a fact about the data rather
// than something the lookup depends on.
const GFXglyph* findGlyph(const TextFace& face, uint32_t codepoint,
                          const GFXfont** font) {
  if (codepoint > 0xFFFF) return nullptr;  // a GFXfont's range is uint16_t

  for (uint8_t i = 0; i < face.fontCount; ++i) {
    const GFXfont* candidate = face.fonts[i];
    const uint16_t first = pgm_read_word(&candidate->first);
    const uint16_t last = pgm_read_word(&candidate->last);
    if (codepoint < first || codepoint > last) continue;

    const GFXglyph* glyphs =
        static_cast<const GFXglyph*>(pgm_read_ptr(&candidate->glyph));
    const GFXglyph* glyph = &glyphs[codepoint - first];
    if (isHole(glyph)) continue;

    *font = candidate;
    return glyph;
  }
  return nullptr;
}

// What actually gets drawn for a code point: the glyph, its font, and the
// code point that reached it, which is kFallback when the face had nothing.
// Both textWidth() and textDraw() go through this, so the width of a string
// is the width of the string that appears.
struct Resolved {
  const GFXfont* font;
  const GFXglyph* glyph;
  uint16_t codepoint;
};

Resolved resolve(const TextFace& face, uint32_t codepoint) {
  Resolved out = {nullptr, nullptr, static_cast<uint16_t>(codepoint)};
  out.glyph = findGlyph(face, codepoint, &out.font);
  if (out.glyph != nullptr) return out;

  out.codepoint = kFallback;
  out.glyph = findGlyph(face, kFallback, &out.font);
  return out;
}

}  // namespace

uint32_t textNextCodepoint(const char** cursor) {
  const uint8_t* p = reinterpret_cast<const uint8_t*>(*cursor);
  const uint8_t lead = *p;
  if (lead == 0) return 0;

  uint8_t extra = 0;
  uint32_t codepoint = 0;
  if (lead < 0x80) {
    *cursor = reinterpret_cast<const char*>(p + 1);
    return lead;
  } else if ((lead & 0xE0) == 0xC0) {
    extra = 1;
    codepoint = lead & 0x1F;
  } else if ((lead & 0xF0) == 0xE0) {
    extra = 2;
    codepoint = lead & 0x0F;
  } else if ((lead & 0xF8) == 0xF0) {
    extra = 3;
    codepoint = lead & 0x07;
  } else {
    // A continuation byte with no lead, or a length UTF-8 does not have.
    *cursor = reinterpret_cast<const char*>(p + 1);
    return kReplacement;
  }

  for (uint8_t i = 1; i <= extra; ++i) {
    if ((p[i] & 0xC0) != 0x80) {
      // Truncated, or a lead byte where a continuation should be. Resume at
      // the offending byte rather than past it -- if it is the terminator,
      // the next call ends the string, and if it is a lead byte it starts
      // the next character.
      *cursor = reinterpret_cast<const char*>(p + i);
      return kReplacement;
    }
    codepoint = (codepoint << 6) | (p[i] & 0x3F);
  }

  *cursor = reinterpret_cast<const char*>(p + 1 + extra);
  // An overlong encoding of NUL would otherwise end the string early.
  return codepoint == 0 ? kReplacement : codepoint;
}

bool textHasGlyph(const TextFace& face, uint32_t codepoint) {
  const GFXfont* font = nullptr;
  return findGlyph(face, codepoint, &font) != nullptr;
}

int32_t textWidth(const TextFace& face, const char* text, uint8_t size) {
  if (text == nullptr) return 0;

  int32_t width = 0;
  const char* cursor = text;
  for (uint32_t codepoint = textNextCodepoint(&cursor); codepoint != 0;
       codepoint = textNextCodepoint(&cursor)) {
    const Resolved resolved = resolve(face, codepoint);
    if (resolved.glyph != nullptr) {
      width += pgm_read_byte(&resolved.glyph->xAdvance);
    }
  }
  return width * size;
}

int32_t textDraw(Seeed_GFX& gfx, const TextFace& face, const char* text,
                 int32_t x, int32_t top, uint8_t size) {
  if (text == nullptr) return x;

  const int32_t baseline = top + static_cast<int32_t>(face.ascent) * size;
  gfx.setTextSize(size);

  const GFXfont* current = nullptr;
  const char* cursor = text;
  for (uint32_t codepoint = textNextCodepoint(&cursor); codepoint != 0;
       codepoint = textNextCodepoint(&cursor)) {
    const Resolved resolved = resolve(face, codepoint);
    if (resolved.glyph == nullptr) continue;

    if (resolved.font != current) {
      // setFreeFont() walks the whole range to measure it, so it is called
      // when the script changes rather than once per character. A Russian
      // sentence changes font at its punctuation and nowhere else.
      gfx.setFreeFont(resolved.font);
      current = resolved.font;
    }

    // The advance is taken from the glyph rather than from drawChar()'s
    // return, so that this loop and textWidth() cannot disagree about where
    // the string ends.
    gfx.drawChar(resolved.codepoint, x, baseline, 1);
    x += static_cast<int32_t>(pgm_read_byte(&resolved.glyph->xAdvance)) * size;
  }

  return x;
}

void textCopy(char* out, size_t size, const char* text) {
  if (out == nullptr || size == 0) return;

  size_t used = 0;
  if (text != nullptr) {
    const char* cursor = text;
    const char* start = cursor;
    while (textNextCodepoint(&cursor) != 0) {
      const size_t bytes = static_cast<size_t>(cursor - start);
      if (used + bytes + 1 > size) break;
      memcpy(out + used, start, bytes);
      used += bytes;
      start = cursor;
    }
  }
  out[used] = '\0';
}
