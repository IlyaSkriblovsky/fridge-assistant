#include "dashboard_png.h"

#include <esp_heap_caps.h>
#include <memory>
#include <miniz.h>
#include <string.h>
#include "dashboard_protocol.h"

namespace dashboardPng {
namespace {
constexpr size_t kRowBytes = dashboardProtocol::kWidth / 8;
constexpr size_t kScanBytes = (kRowBytes + 1) * dashboardProtocol::kHeight;
struct Scratch {
  tinfl_decompressor inflate;
  uint8_t scan[kScanBytes];
};
uint32_t be32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
uint8_t paeth(int a, int b, int c) {
  const int p = a + b - c;
  const int pa = abs(p - a), pb = abs(p - b), pc = abs(p - c);
  return pa <= pb && pa <= pc ? a : pb <= pc ? b : c;
}
}  // namespace

bool decode(uint8_t* png, size_t size, uint8_t* pixels, Cancelled cancelled, void* context) {
  const auto stop = [&]() { return cancelled && cancelled(context); };
  const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (!png || !pixels || size < 8 || size > dashboardProtocol::kMaxPngBytes ||
      memcmp(png, signature, 8) || stop()) return false;

  // Validate every chunk before stripping its wrapper. The compacted IDAT
  // stream stays behind the read cursor, including when IDAT is split.
  size_t offset = 8, compressed = 0;
  bool header = false, idat = false, end = false;
  while (offset < size) {
    if (stop() || size - offset < 12) return false;
    const size_t count = be32(png + offset);
    if (count > size - offset - 12) return false;
    const uint8_t* type = png + offset + 4;
    const uint8_t* data = type + 4;
    if (mz_crc32(0, type, count + 4) != be32(data + count)) return false;
    if (!header) {
      if (memcmp(type, "IHDR", 4) || count != 13 ||
          be32(data) != dashboardProtocol::kWidth ||
          be32(data + 4) != dashboardProtocol::kHeight ||
          data[8] != 1 || data[9] != 0 || data[10] != 0 ||
          data[11] != 0 || data[12] != 0) return false;
      header = true;
    } else if (!memcmp(type, "IDAT", 4)) {
      memmove(png + compressed, data, count);
      compressed += count;
      idat = true;
    } else if (!memcmp(type, "IEND", 4)) {
      if (count || !idat || offset + 12 != size) return false;
      end = true;
    } else {
      // The server emits no ancillary chunks; in particular, tRNS and PLTE
      // must not silently change the interpretation of black/white pixels.
      return false;
    }
    offset += count + 12;
  }
  if (!end || !compressed || stop()) return false;

  std::unique_ptr<Scratch, decltype(&heap_caps_free)> scratch(
      static_cast<Scratch*>(heap_caps_malloc(sizeof(Scratch),
          MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)), heap_caps_free);
  if (!scratch) return false;
  tinfl_init(&scratch->inflate);
  size_t input = 0, output = 0;
  tinfl_status status;
  do {
    if (stop()) return false;
    size_t in = compressed - input;
    // Bound work between cancellation checks, retaining the entire output
    // as the inflater's non-wrapping dictionary.
    size_t out = kScanBytes - output;
    if (out > 1024) out = 1024;
    status = tinfl_decompress(&scratch->inflate, png + input, &in,
        scratch->scan, scratch->scan + output, &out,
        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF);
    input += in;
    output += out;
    if (status == TINFL_STATUS_DONE) break;
    if (status != TINFL_STATUS_HAS_MORE_OUTPUT || (!in && !out)) return false;
  } while (true);
  if (input != compressed || output != kScanBytes || stop()) return false;

  // Filters operate on packed bytes, with a one-byte left neighbour at 1bpp.
  // Keep scanlines in PNG polarity until the following row has used them.
  for (size_t y = 0; y < dashboardProtocol::kHeight; ++y) {
    if (stop()) return false;
    uint8_t* row = scratch->scan + y * (kRowBytes + 1) + 1;
    const uint8_t filter = row[-1];
    if (filter > 4) return false;
    const uint8_t* previous = y ? row - (kRowBytes + 1) : nullptr;
    for (size_t x = 0; x < kRowBytes; ++x) {
      const uint8_t a = x ? row[x - 1] : 0;
      const uint8_t b = previous ? previous[x] : 0;
      const uint8_t c = previous && x ? previous[x - 1] : 0;
      switch (filter) {
        case 1: row[x] += a; break;
        case 2: row[x] += b; break;
        case 3: row[x] += (int(a) + b) / 2; break;
        case 4: row[x] += paeth(a, b, c); break;
      }
      pixels[y * kRowBytes + x] = row[x] ^ 0xFF;
    }
  }
  return !stop();
}
}  // namespace dashboardPng
