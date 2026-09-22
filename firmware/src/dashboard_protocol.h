#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "config.h"

namespace dashboardProtocol {
constexpr size_t kWidth = 800, kHeight = 480, kFrameBytes = kWidth * kHeight / 8;

inline uint32_t interval(const char* text) {
  if (!text || !*text) return config::kDashboardDefaultSeconds;
  uint32_t value = 0;
  for (; *text; ++text) {
    if (*text < '0' || *text > '9') return config::kDashboardDefaultSeconds;
    const unsigned digit = *text - '0';
    if (value > (UINT32_MAX - digit) / 10) return config::kDashboardDefaultSeconds;
    value = value * 10 + digit;
  }
  if (!value) return config::kDashboardDefaultSeconds;
  if (value < config::kDashboardMinSeconds) return config::kDashboardMinSeconds;
  return value > config::kDashboardMaxSeconds ? config::kDashboardMaxSeconds : value;
}

// RTC microseconds, not uptime: both work and intervening sleeps spend the interval.
inline uint64_t sleepUs(uint64_t deadline, uint64_t now) {
  return deadline > now ? deadline - now : config::kDashboardMinSeconds * 1000000ULL;
}

// The fixed buffer is supplied by the caller. Overflow poisons the whole response.
struct Frame {
  uint8_t* data;
  size_t size = 0;
  bool bad = false;
  bool format = false;
  bool type = false;
  uint32_t nextSeconds = config::kDashboardDefaultSeconds;

  void header(const char* key, const char* value) {
    if (!strcasecmp(key, "Dashboard-Format")) {
      if (format || strcmp(value, "mono1-v1")) bad = true;
      format = true;
    } else if (!strcasecmp(key, "Content-Type")) {
      if (type || strcasecmp(value, "application/octet-stream")) bad = true;
      type = true;
    } else if (!strcasecmp(key, "Content-Encoding")) {
      if (strcasecmp(value, "identity")) bad = true;
    } else if (!strcasecmp(key, "Next-Update-After")) {
      nextSeconds = interval(value);
    }
  }
  bool append(const void* bytes, size_t count) {
    if (bad || count > kFrameBytes - size) { bad = true; return false; }
    memcpy(data + size, bytes, count);
    size += count;
    return true;
  }
  bool valid(int status, int64_t declared, bool complete) const {
    return !bad && format && type && status == 200 && complete &&
           declared == static_cast<int64_t>(kFrameBytes) && size == kFrameBytes;
  }
};
}  // namespace dashboardProtocol
