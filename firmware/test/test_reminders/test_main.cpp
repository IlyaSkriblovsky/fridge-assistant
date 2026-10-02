#include "voice_upload_gate.h"
#include <unity.h>
#include "reminder_json.h"
#include <string>
#include "text.h"
#include "reminder_layout.h"
#include "fonts/fonts.h"

void setUp() {}
void tearDown() {}
reminder::Snapshot snapshot(int count = 1) {
  reminder::Snapshot s;
  s.version = 1; s.serverMs = 1000; s.count = count;
  for (int i = 0; i < count; ++i) {
    s.items[i].id = i + 1; s.items[i].due = 2000 + i * 1000;
    s.items[i].created = 1000;
    strcpy(s.items[i].text, "Яйца");
  }
  return s;
}
void sequence_and_offline_read() {
  reminder::State state;
  auto s = snapshot(2);
  TEST_ASSERT_TRUE(state.apply(s));
  TEST_ASSERT_FALSE(state.fire(1999));
  TEST_ASSERT_TRUE(state.fire(2000));
  TEST_ASSERT_EQUAL(1, state.items[state.current()].id);
  TEST_ASSERT_EQUAL(3000, state.nextDue());
  TEST_ASSERT_TRUE(state.fire(3000));
  TEST_ASSERT_EQUAL(2, state.items[state.current()].id);
  TEST_ASSERT_TRUE(state.read());
  TEST_ASSERT_EQUAL(1, state.items[state.current()].id);
  TEST_ASSERT_TRUE(state.apply(s)); // lost ack response: don't resurrect ID 2
  TEST_ASSERT_EQUAL(1, state.count);
  TEST_ASSERT_FALSE(state.fire(5000));
  TEST_ASSERT_TRUE(state.read());
  TEST_ASSERT_EQUAL(-1, state.current());
  TEST_ASSERT_EQUAL(2, state.pendingCount);
  s.version++; s.count = 0; s.ackCount = 2;
  s.acknowledged[0] = 1; s.acknowledged[1] = 2;
  TEST_ASSERT_TRUE(state.apply(s));
  TEST_ASSERT_EQUAL(0, state.pendingCount);
  TEST_ASSERT_TRUE(state.apply(s));
}
void simultaneous_order_and_cancellation() {
  reminder::State state;
  auto s = snapshot(3);
  for (size_t i = 0; i < s.count; ++i) s.items[i].due = 2000;
  s.items[0].created = 1500;
  TEST_ASSERT_TRUE(state.apply(s));
  TEST_ASSERT_TRUE(state.fire(9000)); // cold overdue: one group, all fired
  TEST_ASSERT_FALSE(state.fire(9000));
  TEST_ASSERT_EQUAL(1, state.items[state.current()].id);
  s.version++; s.items[0] = s.items[2]; s.count = 2;
  TEST_ASSERT_TRUE(state.apply(s));
  TEST_ASSERT_EQUAL(3, state.items[state.current()].id);
  TEST_ASSERT_FALSE(state.fire(9000));
  state.read();
  TEST_ASSERT_EQUAL(2, state.items[state.current()].id);
}
void stale_and_queue_capacity() {
  reminder::State state;
  auto s = snapshot(10);
  TEST_ASSERT_TRUE(state.apply(s));
  state.fire(99999);
  for (int i = 0; i < 10; ++i) TEST_ASSERT_TRUE(state.read());
  TEST_ASSERT_EQUAL(10, state.pendingCount);
  auto replacement = snapshot(); replacement.version++;
  replacement.items[0].id = 99;
  TEST_ASSERT_FALSE(state.apply(replacement)); // preserve every pending ack
  TEST_ASSERT_EQUAL(10, state.pendingCount);
  replacement.count = 0; replacement.ackCount = 10;
  for (int i = 0; i < 10; ++i) replacement.acknowledged[i] = i + 1;
  TEST_ASSERT_TRUE(state.apply(replacement));
  TEST_ASSERT_FALSE(state.apply(s));
  replacement.serverMs--;
  TEST_ASSERT_FALSE(state.apply(replacement));
  TEST_ASSERT_EQUAL(0, state.count);
}
void button_hold_and_release() {
  for (int button = 0; button < 3; ++button) {
    reminder::Buttons gate;
    TEST_ASSERT_FALSE(gate.poll(true, 1, 30));
    TEST_ASSERT_TRUE(gate.poll(true, 31, 30));
    TEST_ASSERT_FALSE(gate.poll(true, 1000, 30));
    TEST_ASSERT_FALSE(gate.poll(false, 1001, 30));
    TEST_ASSERT_FALSE(gate.poll(true, 1010, 30)); // bounce, not another press
    gate.poll(false, 1100, 30); gate.poll(false, 1130, 30);
    TEST_ASSERT_FALSE(gate.poll(true, 1200, 30));
    TEST_ASSERT_TRUE(gate.poll(true, 1230, 30));
    gate.armed = false; // held button when voice was interrupted
    TEST_ASSERT_FALSE(gate.poll(true, 2000, 30));
  }
}
void json_validation() {
  JsonDocument doc;
  const char* valid = R"({"version":1,"server_time_ms":1000,"active":[{"id":1,"text":"Яйца","due_at_ms":2000,"created_at_ms":900}],"acknowledged_ids":[]})";
  reminder::Snapshot s;
  TEST_ASSERT_FALSE(deserializeJson(doc, valid));
  TEST_ASSERT_TRUE(reminder::parse(doc.as<JsonVariantConst>(), s));
  TEST_ASSERT_EQUAL_STRING("Яйца", s.items[0].text);
  doc["active"][0]["text"] = std::string(241, 'x');
  TEST_ASSERT_FALSE(reminder::parse(doc.as<JsonVariantConst>(), s));
  deserializeJson(doc, valid); doc["active"][0]["id"] = "1";
  TEST_ASSERT_FALSE(reminder::parse(doc.as<JsonVariantConst>(), s));
  deserializeJson(doc, valid); doc["active"].as<JsonArray>().add(doc["active"][0]);
  TEST_ASSERT_FALSE(reminder::parse(doc.as<JsonVariantConst>(), s));
  deserializeJson(doc, valid); doc.remove("active");
  TEST_ASSERT_FALSE(reminder::parse(doc.as<JsonVariantConst>(), s));
  deserializeJson(doc, valid); doc["active"].clear();
  TEST_ASSERT_FALSE(reminder::parse(doc.as<JsonVariantConst>(), s));
  deserializeJson(doc, valid); doc["active"].to<JsonArray>();
  TEST_ASSERT_TRUE(reminder::parse(doc.as<JsonVariantConst>(), s));
  TEST_ASSERT_EQUAL(0, s.count);
}
void upload_race_and_full_text() {
  VoiceUploadGate before;
  TEST_ASSERT_TRUE(before.interrupt());
  TEST_ASSERT_FALSE(before.commit(false));
  VoiceUploadGate after;
  TEST_ASSERT_TRUE(after.commit(false));
  TEST_ASSERT_FALSE(after.interrupt());
  VoiceUploadGate sameTime;
  TEST_ASSERT_FALSE(sameTime.commit(true));
  // Worst-width text plus the entire interruption label must fit the smallest
  // face; normal reminders use the largest face that fits.
  std::string text(240, 'W');
  text += "\n\nГолосовой запрос прерван";
  const int lines = textWrapLines(fontFreeSans12, text.c_str(), 1, reminderLayout::kWidth);
  TEST_ASSERT_LESS_OR_EQUAL(reminderLayout::kHeight, (lines - 1) * fontFreeSans12.yAdvance + textBoxHeight(fontFreeSans12, 1));
  TEST_ASSERT_FALSE(reminder::validText("a\nb"));
  TEST_ASSERT_FALSE(reminder::validText("\xc0\x80"));
  TEST_ASSERT_FALSE(reminder::validText("\xed\xa0\x80"));
  TEST_ASSERT_TRUE(reminder::validText("Фильтр — νερό"));
}
void repeat_series() {
  reminder::State state;
  auto s = snapshot(2);
  state.apply(s);
  state.fire(2000);
  TEST_ASSERT_TRUE(state.alarm.poll(true, 100, 180));
  TEST_ASSERT_FALSE(state.alarm.poll(false, 279, 180));
  // A successful sync with a different server clock must preserve raw ticks.
  s.serverMs += 100000;
  state.apply(s);
  TEST_ASSERT_EQUAL(280, state.alarm.nextTicks);
  auto retained = state; // RTC state survives deep sleep verbatim.
  for (int i = 1; i < 5; ++i)
    TEST_ASSERT_TRUE(retained.alarm.poll(false, 100 + i * 180, 180));
  TEST_ASSERT_FALSE(retained.alarm.poll(false, 10000, 180));
  TEST_ASSERT_EQUAL(5, retained.alarm.count);
  // New event restarts an exhausted series, even at an overdue repeat.
  TEST_ASSERT_TRUE(retained.fire(3000));
  TEST_ASSERT_TRUE(retained.alarm.poll(true, 10000, 180));
  TEST_ASSERT_EQUAL(1, retained.alarm.count);
  retained.read();
  TEST_ASSERT_EQUAL(10180, retained.alarm.nextTicks);
  TEST_ASSERT_TRUE(retained.alarm.poll(false, 20000, 180));
  TEST_ASSERT_EQUAL(20180, retained.alarm.nextTicks);
  TEST_ASSERT_FALSE(retained.alarm.poll(false, 20000, 180));
  retained.read();
  TEST_ASSERT_EQUAL(0, retained.alarm.nextTicks);
  TEST_ASSERT_EQUAL(0, retained.alarm.count);
  // Cancellation clears the series too, but future reminders remain.
  s.version++; s.items[0] = s.items[1]; s.count = 1;
  state.apply(s);
  TEST_ASSERT_EQUAL(0, state.alarm.nextTicks);
  TEST_ASSERT_EQUAL(0, state.alarm.count);
}
void reminder_layout_bounds() {
  const std::string samples[] = {"Яйца", "Проверить духовку и выключить свет",
    std::string(240, 'W') + "\n\nГолосовой запрос прерван"};
  for (const auto& text : samples) {
    const auto box = reminderLayout::measure(text.c_str());
    TEST_ASSERT_GREATER_OR_EQUAL(reminderLayout::kTop, box.y);
    TEST_ASSERT_LESS_OR_EQUAL(reminderLayout::kTop + reminderLayout::kHeight,
                              box.y + box.height);
    Seeed_GFX gfx(800, 480);
    reminderLayout::draw(gfx, text.c_str());
    for (int y = 0; y < 480; ++y)
      for (int x = 0; x < 800; ++x)
        if (gfx.px[y * 800 + x] == TFT_BLACK) {
          TEST_ASSERT_TRUE(x >= 40 && x < 760);
          TEST_ASSERT_TRUE(y >= reminderLayout::kTop && y < 470);
        }
  }
}
int main() {
  UNITY_BEGIN();
  RUN_TEST(repeat_series);
  RUN_TEST(reminder_layout_bounds);
  RUN_TEST(sequence_and_offline_read);
  RUN_TEST(simultaneous_order_and_cancellation);
  RUN_TEST(stale_and_queue_capacity);
  RUN_TEST(button_hold_and_release);
  RUN_TEST(json_validation);
  RUN_TEST(upload_race_and_full_text);
  return UNITY_END();
}
