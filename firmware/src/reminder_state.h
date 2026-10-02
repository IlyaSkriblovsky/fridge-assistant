#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>

// Plain fixed-size state: host-testable and retained verbatim in RTC memory.
namespace reminder {
constexpr size_t kLimit = 10;
constexpr size_t kTextBytes = 240;
constexpr int64_t kMaxTime = 4102444800000LL;
struct Item {
  int64_t id = 0, due = 0, created = 0;
  char text[kTextBytes + 1] = {};
  bool fired = false;
};
struct Snapshot {
  int64_t version = -1, serverMs = 0;
  Item items[kLimit] = {};
  size_t count = 0;
  int64_t acknowledged[kLimit] = {};
  size_t ackCount = 0;
};
inline bool contains(const int64_t* ids, size_t count, int64_t id) {
  for (size_t i = 0; i < count; ++i) if (ids[i] == id) return true;
  return false;
}
inline bool later(const Item& a, const Item& b) {
  return a.due != b.due ? a.due > b.due :
      a.created != b.created ? a.created > b.created : a.id > b.id;
}
// Raw RTC ticks keep repeats independent of server wall-clock adjustments.
struct Alarm {
  static constexpr uint64_t kIntervalUs = 180000000;
  static constexpr unsigned kLimit = 5;
  unsigned count = 0;
  uint64_t nextTicks = 0;
  bool poll(bool fresh, uint64_t now, uint64_t intervalTicks) {
    if (fresh) count = 0;
    else if (!nextTicks || now < nextTicks) return false;
    ++count;
    nextTicks = count < kLimit ? now + intervalTicks : 0;
    return true;
  }
};
struct State {
  Alarm alarm;
  Item items[kLimit] = {};
  size_t count = 0;
  int64_t pending[kLimit] = {};
  size_t pendingCount = 0;
  int64_t version = -1, sampleMs = 0;
  bool apply(const Snapshot& s) {
    if ((s.version < version || (s.version == version && s.serverMs < sampleMs)) || s.count > kLimit || s.ackCount > kLimit) return false;
    State next{};
    next.version = s.version;
    next.sampleMs = s.serverMs;
    for (size_t i = 0; i < pendingCount; ++i)
      if (!contains(s.acknowledged, s.ackCount, pending[i])) next.pending[next.pendingCount++] = pending[i];
    for (size_t i = 0; i < s.count; ++i) {
      const auto& item = s.items[i];
      if (contains(next.pending, next.pendingCount, item.id)) continue;
      // Keep enough queue space to acknowledge every accepted item offline.
      // A later sync drains cancelled acknowledgements before adding new items.
      if (next.count + next.pendingCount == kLimit) return false;
      auto& added = next.items[next.count++];
      added = item;
      added.fired = false;
      for (size_t j = 0; j < count; ++j)
        if (items[j].id == added.id) added.fired = items[j].fired;
    }
    if (next.current() >= 0) next.alarm = alarm;
    *this = next;
    return true;
  }
  bool due(int64_t now) const {
    for (size_t i = 0; i < count; ++i) if (!items[i].fired && items[i].due <= now) return true;
    return false;
  }
  bool fire(int64_t now) {
    bool changed = false;
    for (size_t i = 0; i < count; ++i)
      if (!items[i].fired && items[i].due <= now) { items[i].fired = true; changed = true; }
    return changed;
  }
  int current() const {
    int result = -1;
    for (size_t i = 0; i < count; ++i)
      if (items[i].fired && (result < 0 || later(items[i], items[result]))) result = i;
    return result;
  }
  bool read() {
    const int index = current();
    if (index < 0 || pendingCount == kLimit) return false;
    pending[pendingCount++] = items[index].id;
    for (size_t i = index + 1; i < count; ++i) items[i - 1] = items[i];
    --count;
    if (current() < 0) alarm = Alarm{};
    return true;
  }
  int64_t nextDue() const {
    int64_t result = kMaxTime;
    for (size_t i = 0; i < count; ++i)
      if (!items[i].fired && items[i].due < result) result = items[i].due;
    return result;
  }
};

// All buttons share one release gate. A hold never dismisses another item or
// falls through into recording/silent-mode after the last acknowledgement.
struct Buttons {
  bool armed = true;
  int64_t released = 0, pressed = 0;
  bool poll(bool down, int64_t now, int64_t debounce) {
    if (!down) {
      pressed = 0;
      if (!released) released = now;
      if (now - released >= debounce) armed = true;
      return false;
    }
    released = 0;
    if (!pressed) pressed = now;
    if (!armed || now - pressed < debounce) return false;
    armed = false;
    return true;
  }
};
}
