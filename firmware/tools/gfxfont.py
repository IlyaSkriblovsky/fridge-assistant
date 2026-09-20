#!/usr/bin/env python3
"""Generates src/fonts/ from the GNU FreeFont TTFs.

Seeed_GFX2 ships FreeSans as GFXFF headers covering 0x20-0x7E, and that range
is the whole of D1: the panel could draw no Cyrillic because the font had none.
The faces themselves are not the problem -- GNU FreeFont covers Latin, Greek
and Cyrillic in one design -- so this regenerates the same TTFs at the same
sizes with the glyphs we actually need, and nothing changes about how the
screens look.

## Why not fontconvert

Adafruit's fontconvert takes a first and a last code point and emits every
glyph between them. Our code points are not contiguous -- Latin ends at 0x7E,
Cyrillic starts at 0x401, and the numero sign sits alone at 0x2116 -- and for
the 8000-odd gaps fontconvert would emit the face's .notdef box, once per gap,
per size. A GFXfont cannot express a hole, so this emits several of them per
face instead, each one tight around a script, and src/text.cpp picks the right
one per character. See the TextFace comment there.

## What it matches

The output format is fontconvert's, because Seeed_GFX2's drawCharGfx reads
that format: glyph bitmaps are 1 bit per pixel packed MSB first and run
bit-continuously across rows, each glyph starting on a byte boundary. Metrics
come out of FreeType the same way fontconvert takes them, at its 141 dpi, so
regenerating 0x20-0x7E reproduces the bundled headers' glyph tables exactly.
The bitmaps differ in about 50 bytes of 7464 -- single edge pixels, from a
newer FreeType rasterising a curve one pixel differently. That is why all
three faces are generated here rather than only the new scripts: one
rasteriser for the whole string, so a stem cannot change width halfway through
a word.

## Running it

    python3 -m venv .venv && .venv/bin/pip install freetype-py
    curl -LO https://ftp.gnu.org/gnu/freefont/freefont-ttf-20120503.zip
    unzip freefont-ttf-20120503.zip
    .venv/bin/python tools/gfxfont.py --ttf-dir freefont-20120503

The output is committed, so this only runs when the glyph set changes.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

try:
    import freetype
except ImportError:  # pragma: no cover - the message is the whole point
    sys.exit("freetype-py is missing: pip install freetype-py")

# fontconvert's resolution. Every metric below is in pixels at this dpi, so
# changing it changes the rendered size of every face.
DPI = 141

# The code points the device can draw, grouped into the ranges that become one
# GFXfont each. A group costs 8 bytes per code point it spans, used or not, so
# the groups are tight around what is in them -- which is why the marks are not
# simply 0x2000-0x2026, and why the numero sign is a group of one.
#
# Gaps inside a group get an empty glyph with a zero xAdvance, which is what
# textGlyph() in src/text.cpp reads as "not in this face". A space is empty
# too but advances, so the two cannot be confused.
GROUPS: list[tuple[str, list[int]]] = [
    # Latin, as before.
    ("Latin", list(range(0x20, 0x7F))),
    # No-break space, guillemets and the degree sign: what a Russian or Greek
    # answer brings from Latin-1.
    ("Punctuation", [0xA0, 0xAB, 0xB0, 0xBB]),
    # Monotonic Greek. 0x38B, 0x38D and 0x3A2 are unassigned in Unicode, so
    # they come out empty and the report says so.
    ("Greek", list(range(0x386, 0x3CF))),
    # Russian, plus the two the alphabet range leaves out.
    ("Cyrillic", [0x401] + list(range(0x410, 0x450)) + [0x451]),
    # Dashes, quotes, bullet and ellipsis.
    ("Marks", [0x2013, 0x2014, 0x2018, 0x2019, 0x201A,
               0x201C, 0x201D, 0x201E, 0x2022, 0x2026]),
    # The numero sign, which Russian uses and Latin-1 has no equivalent for.
    ("Numero", [0x2116]),
]

# One entry per face the firmware draws with. The identifier is what
# src/fonts/fonts.h declares and src/sticky/screen.cpp binds to a role.
FACES = [
    ("FreeSansBold.ttf", 24, "fontFreeSansBold24", "free_sans_bold_24"),
    ("FreeSans.ttf", 24, "fontFreeSans24", "free_sans_24"),
    ("FreeSans.ttf", 18, "fontFreeSans18", "free_sans_18"),
]

# drawCharGfx reads GFXglyph::bitmapOffset with pgm_read_word even though the
# field is a uint32_t, so an offset past 64 KiB silently wraps and the glyph
# draws from the wrong place. Each group has its own bitmap array, so the
# ceiling is per group and we are an order of magnitude under it -- but a face
# added at 48 pt would not be.
MAX_BITMAP_BYTES = 0xFFFF


class Glyph:
    __slots__ = ("codepoint", "offset", "width", "height",
                 "x_advance", "x_offset", "y_offset", "present")

    def __init__(self, codepoint, offset, width, height,
                 x_advance, x_offset, y_offset, present):
        self.codepoint = codepoint
        self.offset = offset
        self.width = width
        self.height = height
        self.x_advance = x_advance
        self.x_offset = x_offset
        self.y_offset = y_offset
        self.present = present


def render_group(face: freetype.Face, codepoints: list[int]) -> tuple[bytearray, list[Glyph], list[int]]:
    """Rasterises one group into fontconvert's packed bitmap and glyph table."""
    first, last = codepoints[0], codepoints[-1]
    wanted = set(codepoints)

    bitmaps = bytearray()
    glyphs: list[Glyph] = []
    missing: list[int] = []
    acc = nbits = 0

    def flush() -> None:
        nonlocal acc, nbits
        if nbits:
            bitmaps.append((acc << (8 - nbits)) & 0xFF)
            acc = nbits = 0

    for codepoint in range(first, last + 1):
        flush()  # every glyph starts on a byte boundary
        offset = len(bitmaps)

        if codepoint not in wanted or face.get_char_index(codepoint) == 0:
            if codepoint in wanted:
                missing.append(codepoint)
            # A hole: no bitmap, and the zero xAdvance is what marks it.
            glyphs.append(Glyph(codepoint, offset, 0, 0, 0, 0, 0, False))
            continue

        face.load_char(codepoint, freetype.FT_LOAD_TARGET_MONO | freetype.FT_LOAD_RENDER)
        slot = face.glyph
        bitmap = slot.bitmap
        rows = [
            [(bitmap.buffer[y * bitmap.pitch + (x >> 3)] >> (7 - (x & 7))) & 1
             for x in range(bitmap.width)]
            for y in range(bitmap.rows)
        ]

        if not any(any(row) for row in rows):
            # A glyph that rasterises to no ink at all -- a space. It carries
            # no bitmap, but it still advances, which is what tells it from a
            # hole. fontconvert writes the same thing.
            glyphs.append(Glyph(codepoint, offset, 0, 0, slot.advance.x >> 6, 0, 1, True))
            continue

        for row in rows:
            for bit in row:
                acc = (acc << 1) | bit
                nbits += 1
                if nbits == 8:
                    bitmaps.append(acc)
                    acc = nbits = 0

        glyphs.append(Glyph(codepoint, offset, bitmap.width, bitmap.rows,
                            slot.advance.x >> 6, slot.bitmap_left,
                            1 - slot.bitmap_top, True))

    flush()
    return bitmaps, glyphs, missing


def c_array(values, per_line: int, formatter) -> str:
    lines = []
    for start in range(0, len(values), per_line):
        chunk = values[start:start + per_line]
        lines.append("    " + " ".join(formatter(v) for v in chunk))
    return "\n".join(lines)


def emit_face(ttf_path: Path, size: int, identifier: str, stem: str,
              out_dir: Path) -> dict:
    face = freetype.Face(str(ttf_path))
    face.set_char_size(size * 64, 0, DPI, 0)
    y_advance = face.size.height >> 6

    groups = []
    report = {"identifier": identifier, "ttf": ttf_path.name, "size": size,
              "y_advance": y_advance, "bitmap_bytes": 0, "slots": 0,
              "glyphs": 0, "missing": [], "groups": []}

    ascent = descent = 0
    for name, codepoints in GROUPS:
        bitmaps, glyphs, missing = render_group(face, codepoints)
        if len(bitmaps) > MAX_BITMAP_BYTES:
            sys.exit(f"{identifier}/{name}: {len(bitmaps)} bytes of bitmap "
                     f"exceeds the {MAX_BITMAP_BYTES} byte pgm_read_word "
                     f"ceiling in drawCharGfx")
        groups.append((name, codepoints[0], codepoints[-1], bitmaps, glyphs))
        report["bitmap_bytes"] += len(bitmaps)
        report["slots"] += len(glyphs)
        report["glyphs"] += sum(1 for g in glyphs if g.present)
        report["missing"] += missing
        report["groups"].append((name, len(glyphs), len(bitmaps)))

        # The face's box, measured the way Seeed_GFX2's setFreeFont measures
        # it, so a string sits where drawString used to put it. Both are taken
        # across every group, or a Cyrillic line would sit lower than a Latin
        # one in the same paragraph.
        for glyph in glyphs:
            if not glyph.present:
                continue
            ascent = max(ascent, -glyph.y_offset)
            descent = max(descent, glyph.height + glyph.y_offset)

    report["ascent"] = ascent
    report["descent"] = descent

    body = [
        "// Generated by tools/gfxfont.py -- do not edit.",
        f"// {ttf_path.name} at {size} pt, {DPI} dpi.",
        "",
        '#include "fonts/fonts.h"',
        "",
        "namespace {",
        "",
    ]

    for name, first, last, bitmaps, glyphs in groups:
        body += [
            f"// U+{first:04X}..U+{last:04X}",
            f"const uint8_t k{name}Bitmaps[] PROGMEM = {{",
            c_array(list(bitmaps), 12, lambda v: f"0x{v:02X},"),
            "};",
            "",
            f"const GFXglyph k{name}Glyphs[] PROGMEM = {{",
        ]
        for glyph in glyphs:
            label = (f"U+{glyph.codepoint:04X}"
                     + (f" '{chr(glyph.codepoint)}'" if 0x21 <= glyph.codepoint < 0x7F else "")
                     + ("" if glyph.present else " (absent)"))
            body.append(
                f"    {{ {glyph.offset:6d}, {glyph.width:3d}, {glyph.height:3d}, "
                f"{glyph.x_advance:3d}, {glyph.x_offset:4d}, {glyph.y_offset:4d} }},"
                f"  // {label}"
            )
        body += [
            "};",
            "",
            f"const GFXfont k{name} PROGMEM = {{",
            f"    (uint8_t*)k{name}Bitmaps, (GFXglyph*)k{name}Glyphs,",
            f"    0x{first:04X}, 0x{last:04X}, {y_advance}",
            "};",
            "",
        ]

    body += [
        "const GFXfont* const kFonts[] = {",
    ]
    body += [f"    &k{name}," for name, *_ in groups]
    body += [
        "};",
        "",
        "}  // namespace",
        "",
        f"const TextFace {identifier} = {{",
        f"    kFonts, {len(groups)}, {ascent}, {descent}, {y_advance}",
        "};",
        "",
    ]

    (out_dir / f"{stem}.cpp").write_text("\n".join(body))
    return report


def emit_header(out_dir: Path) -> None:
    lines = [
        "// Generated by tools/gfxfont.py -- do not edit.",
        "//",
        "// The faces the screens draw with, each one GNU FreeFont covering",
        "// Latin, Greek, Cyrillic and the punctuation those two bring. What a",
        "// face is made of, and why it is several GFXfonts, is in src/text.h.",
        "",
        "#pragma once",
        "",
        '#include "text.h"',
        "",
    ]
    for _, _, identifier, _ in FACES:
        lines.append(f"extern const TextFace {identifier};")
    lines.append("")
    (out_dir / "fonts.h").write_text("\n".join(lines))


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ttf-dir", required=True, type=Path,
                        help="the unpacked freefont-ttf directory")
    parser.add_argument("--out", type=Path,
                        default=Path(__file__).resolve().parent.parent / "src" / "fonts",
                        help="where the generated sources go")
    args = parser.parse_args()

    args.out.mkdir(parents=True, exist_ok=True)
    emit_header(args.out)

    total = 0
    for ttf, size, identifier, stem in FACES:
        report = emit_face(args.ttf_dir / ttf, size, identifier, stem, args.out)
        total += report["bitmap_bytes"] + report["slots"] * 8
        print(f"{identifier}: {report['glyphs']} glyphs in {report['slots']} slots, "
              f"{report['bitmap_bytes']} B bitmap + {report['slots'] * 8} B glyphs, "
              f"ascent {report['ascent']} descent {report['descent']} "
              f"yAdvance {report['y_advance']}")
        for name, slots, nbytes in report["groups"]:
            print(f"    {name:<12} {slots:4d} slots {nbytes:6d} B")
        if report["missing"]:
            print("    absent from the face: "
                  + " ".join(f"U+{c:04X}" for c in report["missing"]))
    print(f"total {total} B ({total / 1024:.1f} KiB) of flash")


if __name__ == "__main__":
    main()
