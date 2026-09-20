# Screen preview

Draws the screens on the host, with the firmware's own `src/text.cpp` and the
generated `src/fonts/`, and writes them out as images. It is the host half of
the fonts step the way `e7_pcap.py` is the host half of E7.

```
c++ -std=c++17 -I. -I../../src -o preview preview.cpp \
    ../../src/text.cpp ../../src/fonts/*.cpp
./preview
```

Two PGMs come out: `screens.pgm`, which is the four screens one under the
other, and `glyphs.pgm`, which is every script the faces carry. `preview`
also prints the width of each string, the ink bounds of the word band, and
what `textCopy()` does at each length -- the numbers in
[S13](../../docs/implementation.md) came from here.

## What it does not prove

`Seeed_GFX.h` here is a stub: a framebuffer, and `drawChar()` transcribed from
Seeed_GFX2's `drawCharGfx()`. So this checks the layout, the glyphs, the
script switching and the arithmetic, and it checks them against the real font
data -- but it is a second copy of the library's blitter and could drift from
it. It cannot say anything at all about the panel: contrast at 235 dpi,
whether a partial refresh of the word band lands cleanly, ghosting. **A step
is still done when it has run on the device.**

Nothing in here is built by PlatformIO or by CI; it is not under `src/`.
