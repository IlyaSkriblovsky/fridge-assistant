#pragma once
#include <ArduinoJson.h>
#include "reminder_state.h"

namespace reminder {
inline bool validText(const char* text) {
  const auto* p = reinterpret_cast<const unsigned char*>(text);
  while (*p) {
    const unsigned char first = *p++;
    if (first < 32 || first == 127) return false;
    if (first < 128) continue;
    const int extra = first >= 0xc2 && first <= 0xdf ? 1 :
        first >= 0xe0 && first <= 0xef ? 2 : first >= 0xf0 && first <= 0xf4 ? 3 : -1;
    if (extra < 0) return false;
    uint32_t cp = first & (0x7f >> (extra + 1));
    for (int i = 0; i < extra; ++i) {
      if (*p < 0x80 || *p > 0xbf) return false;
      cp = (cp << 6) | (*p++ & 0x3f);
    }
    if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) ||
        (extra == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
  }
  return true;
}
inline bool parse(JsonVariantConst json, Snapshot& out) {
  if (!json.is<JsonObjectConst>() || !json["version"].is<int64_t>() ||
      !json["server_time_ms"].is<int64_t>() || !json["active"].is<JsonArrayConst>() ||
      !json["acknowledged_ids"].is<JsonArrayConst>()) return false;
  out = Snapshot{};
  Snapshot& s = out;
  s.version = json["version"].as<int64_t>();
  s.serverMs = json["server_time_ms"].as<int64_t>();
  if (s.version < 0 || s.serverMs <= 0 || s.serverMs >= kMaxTime) return false;
  for (JsonVariantConst row : json["active"].as<JsonArrayConst>()) {
    if (s.count == kLimit || !row["id"].is<int64_t>() || !row["due_at_ms"].is<int64_t>() ||
        !row["created_at_ms"].is<int64_t>() || !row["text"].is<const char*>()) return false;
    auto& item = s.items[s.count++];
    item.id = row["id"].as<int64_t>();
    item.due = row["due_at_ms"].as<int64_t>();
    item.created = row["created_at_ms"].as<int64_t>();
    JsonString text = row["text"].as<JsonString>();
    if (item.id <= 0 || item.due <= 0 || item.due >= kMaxTime || item.created <= 0 ||
        item.created >= kMaxTime || text.size() == 0 || text.size() > kTextBytes ||
        strlen(text.c_str()) != text.size() || !validText(text.c_str())) return false;
    memcpy(item.text, text.c_str(), text.size() + 1);
    for (size_t i = 0; i + 1 < s.count; ++i) if (s.items[i].id == item.id) return false;
  }
  for (JsonVariantConst id : json["acknowledged_ids"].as<JsonArrayConst>()) {
    if (s.ackCount == kLimit || !id.is<int64_t>() || id.as<int64_t>() <= 0) return false;
    s.acknowledged[s.ackCount++] = id.as<int64_t>();
  }
  return true;
}
}
