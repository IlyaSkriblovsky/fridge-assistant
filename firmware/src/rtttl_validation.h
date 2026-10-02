#pragma once
#include <stddef.h>
#include <string.h>

// Strict standard RTTTL subset accepted by E17 before handing text to the
// library (whose parser assumes valid, terminated input). No RTX extensions.
namespace rtttl {
constexpr size_t kMaxBytes = 2048;
inline bool number(const char*& p, unsigned& n) {
  n = 0;
  const char* start = p;
  while (*p >= '0' && *p <= '9') {
    n = n * 10 + (*p++ - '0');
    if (n > 10000) return false;
  }
  return p != start;
}
inline bool duration(unsigned n) {
  return n == 1 || n == 2 || n == 4 || n == 8 || n == 16 || n == 32;
}
inline bool valid(const char* song, int shift = 0) {
  if (!song || strlen(song) > kMaxBytes || shift < -3 || shift > 3) return false;
  const char* p = song;
  size_t name = 0;
  while (*p && *p != ':') {
    if (*p < 32 || *p > 126) return false;
    ++p; ++name;
  }
  if (!name || *p++ != ':') return false;
  unsigned octave = 6, beat = 63, def = 4, seen = 0;
  while (*p != ':') {
    const char key = *p++;
    const unsigned mask = key == 'd' ? 1 : key == 'o' ? 2 : key == 'b' ? 4 : 0;
    if (!mask || (seen & mask) || *p++ != '=') return false;
    seen |= mask;
    unsigned n;
    const char* digits = p;
    if (!number(p, n)) return false;
    if (key == 'd') { if (!duration(n)) return false; def = n; }
    if (key == 'o') { if (p - digits != 1 || n > 7) return false; octave = n; }
    if (key == 'b') { if (n < 25 || n > 900) return false; beat = n; }
    if (*p == ':') break;
    if (*p++ != ',') return false;
  }
  if (*p++ != ':' || !*p) return false;
  unsigned count = 0, total = 0;
  while (*p) {
    unsigned d = def, o = octave;
    if (*p >= '0' && *p <= '9' && (!number(p, d) || !duration(d))) return false;
    const char note = *p++;
    if (!note || !strchr("abcdefgp", note)) return false;
    if (*p == '#') { if (note == 'p' || note == 'b' || note == 'e') return false; ++p; }
    bool dot = false;
    if (*p == '.') { dot = true; ++p; }
    if (*p >= '0' && *p <= '9') {
      o = *p++ - '0';
      if (o > 7) return false;
    }
    if (*p == '.') { if (dot) return false; dot = true; ++p; }
    if (note != 'p' && (int(o) + shift < 0 || int(o) + shift > 7)) return false;
    unsigned ms = (60000 / beat) * 4 / d;
    total += ms + (dot ? ms / 2 : 0);
    if (++count > 512 || total > 120000) return false;
    if (!*p) break;
    if (*p++ != ',' || !*p) return false;
  }
  return count > 0;
}
}
