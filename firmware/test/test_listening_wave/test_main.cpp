#include <unity.h>
#include "listening_wave.h"

void setUp() {}
void tearDown() {}

void animation_preserves_every_pixel_outside_its_window() {
  Seeed_GFX panel(800, 480);
  // A patterned surrounding screen catches white clears as well as black ink.
  for (size_t i = 0; i < panel.px.size(); ++i) panel.px[i] = i % 3 ? TFT_WHITE : TFT_BLACK;
  const auto before = panel.px;
  for (uint8_t frame = 0; frame < listeningWave::kFrames; ++frame) {
    listeningWave::draw(panel, frame);
    for (int y = 0; y < 480; ++y)
      for (int x = 0; x < 800; ++x) {
        if (x >= listeningWave::kX && x < listeningWave::kX + listeningWave::kWidth &&
            y >= listeningWave::kY && y < listeningWave::kY + listeningWave::kHeight) continue;
        TEST_ASSERT_EQUAL_UINT16(before[y * 800 + x], panel.px[y * 800 + x]);
      }
  }
}

void every_transition_removes_all_previous_frame_pixels() {
  for (uint8_t from = 0; from < listeningWave::kFrames; ++from)
    for (uint8_t to = 0; to < listeningWave::kFrames; ++to) {
      Seeed_GFX actual(800, 480), expected(800, 480);
      listeningWave::draw(actual, from);
      const auto previous = actual.px;
      listeningWave::draw(actual, to);
      listeningWave::draw(expected, to);
      TEST_ASSERT_TRUE(actual.px == expected.px);
      if (from != to) TEST_ASSERT_TRUE(actual.px != previous);
    }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(animation_preserves_every_pixel_outside_its_window);
  RUN_TEST(every_transition_removes_all_previous_frame_pixels);
  return UNITY_END();
}
