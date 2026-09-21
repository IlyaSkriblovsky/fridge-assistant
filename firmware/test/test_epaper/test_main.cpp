#include <unity.h>
#include "sticky/epaper.h"

// A fixture gives Unity's tearDown ownership even when an assertion aborts a
// test, so the production driver's allocated shadow is always released.
static Driver_SSD1677_Sticky* driver;
static uint8_t expectedOld[48000];
static uint8_t expectedNew[48000];

void setUp() {
  testHeap::failAllocation = false;
  driver = new Driver_SSD1677_Sticky;
  memset(expectedOld, 0xff, sizeof(expectedOld));
  memset(expectedNew, 0xff, sizeof(expectedNew));
}
void tearDown() { delete driver; }

static void check_transition(bool wasOn, int x, int y) {
  auto& d = *driver;
  const uint8_t ink[] = {0x81, 0x42, 0x24, 0x18};
  const uint8_t white[] = {0, 0, 0, 0};
  const auto* oldPixels = wasOn ? ink : white;
  const auto* newPixels = wasOn ? white : ink;

  TEST_ASSERT_TRUE(d.beginShadowPrime());
  d.wakePartial();
  d.setAddrWindow(x, y, x + 15, y + 1);
  d.pushNewColors(oldPixels, 4);
  d.updatePartial();
  TEST_ASSERT_EQUAL_UINT(0, d.updates);
  TEST_ASSERT_EQUAL_UINT(0, d.bus.imageBytes);

  d.endShadowPrime();
  d.wakePartial();
  d.setAddrWindow(x, y, x + 15, y + 1);
  d.pushNewColors(newPixels, 4);
  d.updatePartial();
  TEST_ASSERT_EQUAL_UINT(1, d.updates);
  TEST_ASSERT_EQUAL_UINT(96008, d.bus.imageBytes);

  // The entire controller RAM must be initialized, with differences confined
  // to the window. Distinct bytes catch polarity, row stride and address errors.
  for (int i = 0; i < 4; ++i) {
    const int address = (y + i / 2) * 100 + x / 8 + i % 2;
    expectedOld[address] = static_cast<uint8_t>(~oldPixels[i]);
    expectedNew[address] = static_cast<uint8_t>(~newPixels[i]);
  }
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedOld, d.bus.oldRam.data(), sizeof(expectedOld));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedNew, d.bus.newRam.data(), sizeof(expectedNew));

  // Next partial must advance the previous image and write only the window,
  // without seeding both full planes again.
  d.setAddrWindow(x, y, x + 15, y + 1);
  d.pushNewColors(oldPixels, 4);
  d.updatePartial();
  TEST_ASSERT_EQUAL_UINT(2, d.updates);
  TEST_ASSERT_EQUAL_UINT(96016, d.bus.imageBytes);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedNew, d.bus.oldRam.data(), sizeof(expectedNew));
  TEST_ASSERT_EQUAL_HEX8_ARRAY(expectedOld, d.bus.newRam.data(), sizeof(expectedOld));
}

void icon_turns_on() { check_transition(false, 176, 8); }
void icon_turns_off() { check_transition(true, 176, 8); }
void top_left_turns_on() { check_transition(false, 0, 0); }
void top_left_turns_off() { check_transition(true, 0, 0); }
void bottom_right_turns_on() { check_transition(false, 784, 478); }
void bottom_right_turns_off() { check_transition(true, 784, 478); }

int main() {
  UNITY_BEGIN();
  RUN_TEST(icon_turns_on);
  RUN_TEST(icon_turns_off);
  RUN_TEST(top_left_turns_on);
  RUN_TEST(top_left_turns_off);
  RUN_TEST(bottom_right_turns_on);
  RUN_TEST(bottom_right_turns_off);
  return UNITY_END();
}
