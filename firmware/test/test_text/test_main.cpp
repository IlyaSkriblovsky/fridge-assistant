#include <unity.h>
#include <cstring>
#include "text.h"
#include "fonts/fonts.h"

void setUp() {}
void tearDown() {}

void decode_mixed_scripts_and_four_byte_character() {
  const char* cursor = u8"AЯλ…😀";
  for (uint32_t expected : {0x41u, 0x42fu, 0x3bbu, 0x2026u, 0x1f600u})
    TEST_ASSERT_EQUAL_UINT32(expected, textNextCodepoint(&cursor));
  const char* end = cursor;
  TEST_ASSERT_EQUAL_UINT32(0, textNextCodepoint(&cursor));
  TEST_ASSERT_EQUAL_PTR(end, cursor);
}

void broken_utf8_recovers_without_losing_following_text() {
  for (const char* bad : {"\x80", "\xff", "\xc2", "\xe2\x82", "\xf0\x9f\x98", "\xc0\x80"}) {
    const char* cursor = bad;
    TEST_ASSERT_EQUAL_UINT32(0xfffd, textNextCodepoint(&cursor));
    TEST_ASSERT_EQUAL_UINT32(0, textNextCodepoint(&cursor));
  }
  const char* cursor = "\xe2\x82" "A";
  TEST_ASSERT_EQUAL_UINT32(0xfffd, textNextCodepoint(&cursor));
  TEST_ASSERT_EQUAL_UINT32('A', textNextCodepoint(&cursor));
  TEST_ASSERT_EQUAL_UINT32(0, textNextCodepoint(&cursor));
}

void copy_preserves_character_boundaries_at_every_buffer_size() {
  const char* source = u8"AЯλ…😀";
  const size_t boundaries[] = {0, 1, 3, 5, 8, 12};
  for (size_t size = 1; size <= 14; ++size) {
    char out[16];
    std::memset(out, '#', sizeof(out));
    textCopy(out, size, source);
    size_t expected = 0;
    for (size_t boundary : boundaries)
      if (boundary < size) expected = boundary;
    TEST_ASSERT_EQUAL_UINT(expected, std::strlen(out));
    if (expected) TEST_ASSERT_EQUAL_MEMORY(source, out, expected);
    TEST_ASSERT_EQUAL_CHAR('#', out[size]);
  }
  char out[] = "sentinel";
  textCopy(out, 0, source);
  TEST_ASSERT_EQUAL_STRING("sentinel", out);
  textCopy(out, sizeof(out), nullptr);
  TEST_ASSERT_EQUAL_STRING("", out);
  textCopy(nullptr, 5, source);
}

void every_face_supports_our_scripts_and_falls_back_per_character() {
  for (const TextFace* face : {&fontFreeSans12, &fontFreeSans18, &fontFreeSans24,
                              &fontFreeSansBold12, &fontFreeSansBold24}) {
    for (uint32_t cp : {0x41u, 0x401u, 0x44fu, 0x3bbu, 0x2026u, 0x2116u})
      TEST_ASSERT_TRUE(textHasGlyph(*face, cp));
    TEST_ASSERT_FALSE(textHasGlyph(*face, 0x1f600));
    TEST_ASSERT_EQUAL_INT(textWidth(*face, "?", 1), textWidth(*face, u8"😀", 1));
    TEST_ASSERT_GREATER_THAN(0, textWidth(*face, u8"Яλ", 1));
    TEST_ASSERT_EQUAL_INT(2 * textWidth(*face, u8"AЯλ", 1),
                         textWidth(*face, u8"AЯλ", 2));
    TEST_ASSERT_EQUAL_INT(0, textWidth(*face, nullptr, 1));
    TEST_ASSERT_EQUAL_INT(0, textWidth(*face, "", 1));
  }
}

void wrapping_handles_spaces_newlines_and_long_utf8_words() {
  const auto& face = fontFreeSans12;
  const int width = textWidth(face, "AA", 1);
  TEST_ASSERT_EQUAL_INT(2, textWrapLines(face, "AA AA", 1, width));
  TEST_ASSERT_EQUAL_INT(2, textWrapLines(face, "  AA   AA  ", 1, width));
  TEST_ASSERT_EQUAL_INT(3, textWrapLines(face, "A\n\nA", 1, 1000));
  TEST_ASSERT_EQUAL_INT(1, textWrapLines(face, "AA", 1, width));
  TEST_ASSERT_EQUAL_INT(2, textWrapLines(face, "AAA", 1, width));
  TEST_ASSERT_EQUAL_INT(2, textWrapLines(face, u8"ЯЯЯ", 1, textWidth(face, u8"ЯЯ", 1)));
  TEST_ASSERT_EQUAL_INT(3, textWrapLines(face, u8"ЯλA", 1, 0));
  TEST_ASSERT_EQUAL_INT(0, textWrapLines(face, "   ", 1, width));
  TEST_ASSERT_EQUAL_INT(0, textWrapLines(face, "", 1, width));
  TEST_ASSERT_EQUAL_INT(0, textWrapLines(face, nullptr, 1, width));
}

void drawing_matches_measurement_and_fallback() {
  const auto& face = fontFreeSans12;
  Seeed_GFX actual(400, 80), expected(400, 80);
  const int end = textDraw(actual, face, u8"AЯλ😀", 7, 3, 1);
  TEST_ASSERT_EQUAL_INT(7 + textWidth(face, u8"AЯλ😀", 1), end);
  textDraw(expected, face, u8"AЯλ?", 7, 3, 1);
  TEST_ASSERT_TRUE(actual.px == expected.px);
  TEST_ASSERT_EQUAL_INT(7, textDraw(actual, face, nullptr, 7, 3, 1));
}

void truncated_last_line_draws_ellipsis_and_respects_line_limit() {
  const auto& face = fontFreeSans12;
  const int width = textWidth(face, "AAAA", 1);
  Seeed_GFX actual(400, 120), expected(400, 120);
  TEST_ASSERT_EQUAL_INT(1, textDrawWrapped(actual, face, "A AAAA", 4, 2, 1, width, 1));
  textDraw(expected, face, u8"A…", 4, 2, 1);
  TEST_ASSERT_TRUE(actual.px == expected.px);

  actual.fillScreen(TFT_WHITE);
  expected.fillScreen(TFT_WHITE);
  TEST_ASSERT_EQUAL_INT(2, textDrawWrapped(actual, face, "AA AA", 4, 2, 1,
                                          textWidth(face, "AA", 1), 2));
  textDraw(expected, face, "AA", 4, 2, 1);
  textDraw(expected, face, "AA", 4, 2 + face.yAdvance, 1);
  TEST_ASSERT_TRUE(actual.px == expected.px);

  actual.fillScreen(TFT_WHITE);
  expected.fillScreen(TFT_WHITE);
  TEST_ASSERT_EQUAL_INT(0, textDrawWrapped(actual, face, "A", 4, 2, 1, width, 0));
  TEST_ASSERT_TRUE(actual.px == expected.px);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(decode_mixed_scripts_and_four_byte_character);
  RUN_TEST(broken_utf8_recovers_without_losing_following_text);
  RUN_TEST(copy_preserves_character_boundaries_at_every_buffer_size);
  RUN_TEST(every_face_supports_our_scripts_and_falls_back_per_character);
  RUN_TEST(wrapping_handles_spaces_newlines_and_long_utf8_words);
  RUN_TEST(drawing_matches_measurement_and_fallback);
  RUN_TEST(truncated_last_line_draws_ellipsis_and_respects_line_limit);
  return UNITY_END();
}
