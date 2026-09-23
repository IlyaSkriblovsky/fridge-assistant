#pragma once

#include <stddef.h>
#include <stdint.h>

namespace dashboardPng {
// Only the server's Pillow mode-1 PNG: 800x480, grayscale, no interlace or
// transparency, IHDR/IDAT/IEND chunks. Input is compacted in place. Output is
// the existing MSB-first, 1=black bitmap and is usable only on success.
// Temporary inflate state and scanlines live in PSRAM, never the worker stack.
using Cancelled = bool (*)(void*);
bool decode(uint8_t* png, size_t size, uint8_t* pixels,
            Cancelled cancelled = nullptr, void* context = nullptr);
}  // namespace dashboardPng
