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

// Ends the last line of an answer with more to it than the panel had room
// for. U+2026 rather than three periods: tools/gfxfont.py puts it in every
// face, and it is narrower than the three would be. Spelled as an escape so
// that no code in this file depends on being read as UTF-8.
constexpr const char* kEllipsis = "\u2026";

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
// Measuring and drawing both go through this -- advanceOf() below is the one
// path to a width -- so the width of a string is the width of the string that
// appears.
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

// How far one character moves the cursor, substitution included. Everything
// that measures goes through this and so does everything that draws, which is
// what keeps a line from being measured as one width and drawn as another.
int32_t advanceOf(const TextFace& face, uint32_t codepoint, uint8_t size) {
  const Resolved resolved = resolve(face, codepoint);
  if (resolved.glyph == nullptr) return 0;
  return static_cast<int32_t>(pgm_read_byte(&resolved.glyph->xAdvance)) * size;
}

// A run of a string: where it starts and how many bytes of it are drawn. A
// wrapped line is a run of the answer rather than a copy of part of it, which
// is what keeps the wrapping from needing a buffer of its own.
struct Run {
  const char* text;
  size_t bytes;
};

// The whole of a null-terminated string, as a run.
Run wholeOf(const char* text) { return {text, text == nullptr ? 0 : strlen(text)}; }

int32_t widthOf(const TextFace& face, const Run& run, uint8_t size) {
  int32_t width = 0;
  const char* cursor = run.text;
  const char* const end = run.text + run.bytes;
  while (cursor < end) {
    const uint32_t codepoint = textNextCodepoint(&cursor);
    if (codepoint == 0) break;
    width += advanceOf(face, codepoint, size);
  }
  return width;
}

// Draws a run from its left edge along the baseline, and returns where the
// next character would go. The caller has already called setTextSize().
int32_t drawRun(Seeed_GFX& gfx, const TextFace& face, const Run& run, int32_t x,
                int32_t baseline, uint8_t size) {
  const GFXfont* current = nullptr;
  const char* cursor = run.text;
  const char* const end = run.text + run.bytes;
  while (cursor < end) {
    const uint32_t codepoint = textNextCodepoint(&cursor);
    if (codepoint == 0) break;

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
    // return, so that this loop and widthOf() cannot disagree about where the
    // run ends.
    gfx.drawChar(resolved.codepoint, x, baseline, 1);
    x += advanceOf(face, codepoint, size);
  }
  return x;
}

// Past the spaces at `p`. A line breaks at a space, and the space belongs to
// the break rather than to the line on either side of it.
const char* pastSpaces(const char* p) {
  while (*p == ' ') ++p;
  return p;
}

// A run without the spaces it ends in -- the ones a break leaves behind, and
// the ones a string can simply end in. A space is one byte and cannot be part
// of a multi-byte character, so walking back over bytes is safe here.
Run trimmed(const char* start, const char* end) {
  while (end > start && end[-1] == ' ') --end;
  return {start, static_cast<size_t>(end - start)};
}

// The next line of the string at `cursor` that fits in `maxWidth`, with
// `cursor` left where the line after it starts. False when the string is
// spent, which is the only way the loops below end.
//
// `reserve` is width held back at the right edge for something the caller
// draws there -- the ellipsis, and nothing else so far. Held back rather than
// subtracted afterwards, so the line ends at a word rather than at whatever
// the ellipsis happened to cover.
//
// A line too narrow for one character and the reserve together still gets its
// character, so the ellipsis after it sits past the edge. That takes a box
// about 60 px wide; the answer's is 720.
bool nextLine(const TextFace& face, const char** cursor, uint8_t size,
              int32_t maxWidth, int32_t reserve, Run* line) {
  const char* p = pastSpaces(*cursor);
  if (*p == '\0') return false;

  const char* const start = p;
  const char* fitEnd = nullptr;   // the last space that fit: where the line ends
  const char* fitNext = nullptr;  // and where the line after it starts
  const int32_t limit = maxWidth - reserve;
  int32_t width = 0;
  bool afterSpace = false;

  for (;;) {
    const char* const charStart = p;
    const uint32_t codepoint = textNextCodepoint(&p);

    // The string's own breaks. textNextCodepoint() does not move past the
    // terminator, so `charStart` is it and the next call ends the loop above.
    if (codepoint == 0 || codepoint == '\n') {
      *cursor = codepoint == 0 ? charStart : p;
      *line = trimmed(start, charStart);
      return true;
    }

    if (codepoint == ' ') {
      // Somewhere the line can end, if what comes after it does not fit. A run
      // of spaces is one such place and not several: the line ends where the
      // first of them starts. The space's own width is counted because two
      // words with a space between them are wider than the two words.
      if (!afterSpace) fitEnd = charStart;
      afterSpace = true;
      fitNext = p;
      width += advanceOf(face, codepoint, size);
      continue;
    }
    afterSpace = false;

    width += advanceOf(face, codepoint, size);
    if (width <= limit) continue;

    if (fitEnd != nullptr) {
      *cursor = fitNext;
      *line = {start, static_cast<size_t>(fitEnd - start)};
      return true;
    }

    // A word wider than the whole line. It breaks inside itself, after the
    // last character that fit, because there is nowhere else to break it and
    // a line that cannot end is a loop. `charStart == start` is the case where
    // not even one character fits, which takes a maxWidth narrower than a
    // glyph -- the character still gets its line, for the same reason.
    *cursor = charStart == start ? p : charStart;
    *line = {start, static_cast<size_t>(*cursor - start)};
    return true;
  }
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
  return widthOf(face, wholeOf(text), size);
}

int32_t textDraw(Seeed_GFX& gfx, const TextFace& face, const char* text,
                 int32_t x, int32_t top, uint8_t size) {
  if (text == nullptr) return x;

  gfx.setTextSize(size);
  return drawRun(gfx, face, wholeOf(text), x,
                 top + static_cast<int32_t>(face.ascent) * size, size);
}

int32_t textWrapLines(const TextFace& face, const char* text, uint8_t size,
                      int32_t maxWidth) {
  if (text == nullptr) return 0;

  int32_t lines = 0;
  const char* cursor = text;
  Run line = {nullptr, 0};
  while (nextLine(face, &cursor, size, maxWidth, 0, &line)) ++lines;
  return lines;
}

int32_t textDrawWrapped(Seeed_GFX& gfx, const TextFace& face, const char* text,
                        int32_t x, int32_t top, uint8_t size, int32_t maxWidth,
                        int32_t maxLines) {
  if (text == nullptr) return 0;

  gfx.setTextSize(size);
  const int32_t baseline = top + static_cast<int32_t>(face.ascent) * size;
  const int32_t lineHeight = static_cast<int32_t>(face.yAdvance) * size;

  int32_t drawn = 0;
  const char* cursor = text;
  Run line = {nullptr, 0};
  while (drawn < maxLines && nextLine(face, &cursor, size, maxWidth, 0, &line)) {
    const int32_t y = baseline + drawn * lineHeight;
    ++drawn;

    if (drawn < maxLines || *pastSpaces(cursor) == '\0') {
      drawRun(gfx, face, line, x, y, size);
      continue;
    }

    // The last line there is room for, and the string still has something to
    // say. It is laid out a second time against the room the ellipsis needs,
    // from where it started -- rather than reserving that room on every line,
    // which would narrow the ones that did not need it, or appending the
    // ellipsis to a full line, which would put it past the margin.
    const char* from = line.text;
    const int32_t reserve = textWidth(face, kEllipsis, size);
    nextLine(face, &from, size, maxWidth, reserve, &line);
    const int32_t end = drawRun(gfx, face, line, x, y, size);
    drawRun(gfx, face, wholeOf(kEllipsis), end, y, size);
  }

  return drawn;
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
