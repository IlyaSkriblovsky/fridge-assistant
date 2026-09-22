#include <unity.h>
#include <vector>
#include "dashboard_protocol.h"

using namespace dashboardProtocol;
void setUp() {}
void tearDown() {}

void frame_is_atomic_and_bounded() {
  std::vector<uint8_t> pixels(kFrameBytes + 1, 0xA5);
  std::vector<uint8_t> body(kFrameBytes, 0x80);
  Frame frame{pixels.data()};
  frame.header("Content-Type", "application/octet-stream");
  frame.header("dashboard-format", "mono1-v1");
  TEST_ASSERT_TRUE(frame.append(body.data(), 1024));
  TEST_ASSERT_FALSE(frame.valid(200, kFrameBytes, true));
  TEST_ASSERT_TRUE(frame.append(body.data() + 1024, kFrameBytes - 1024));
  TEST_ASSERT_FALSE(frame.valid(200, kFrameBytes, false));
  TEST_ASSERT_FALSE(frame.valid(500, kFrameBytes, true));
  TEST_ASSERT_FALSE(frame.valid(200, kFrameBytes + 1, true));
  TEST_ASSERT_FALSE(frame.valid(200, 0, true));
  TEST_ASSERT_TRUE(frame.valid(200, kFrameBytes, true));
  TEST_ASSERT_EQUAL_MEMORY(body.data(), pixels.data(), kFrameBytes);
  TEST_ASSERT_FALSE(frame.append(body.data(), 1));
  TEST_ASSERT_EQUAL_HEX8(0xA5, pixels[kFrameBytes]);
  TEST_ASSERT_FALSE(frame.valid(200, kFrameBytes, true));
}

void unsupported_and_encoded_frames_are_rejected() {
  uint8_t byte = 0;
  for (const char* format : {"png", "mono1-v2", ""}) {
    Frame frame{&byte};
    frame.header("Dashboard-Format", format);
    TEST_ASSERT_TRUE(frame.bad);
  }
  Frame frame{&byte};
  TEST_ASSERT_FALSE(frame.valid(200, kFrameBytes, true));
  frame.header("Content-Encoding", "gzip");
  TEST_ASSERT_TRUE(frame.bad);
  Frame duplicate{&byte};
  duplicate.header("Dashboard-Format", "mono1-v1");
  duplicate.header("Dashboard-Format", "mono1-v1");
  TEST_ASSERT_TRUE(duplicate.bad);
}

void intervals_fall_back_or_clamp_without_overflow() {
  for (const char* value : {"", "0", "-1", "1.5", " 60", "60x", "4294967296", "999999999999999999999"})
    TEST_ASSERT_EQUAL_UINT32(3600, interval(value));
  TEST_ASSERT_EQUAL_UINT32(3600, interval(nullptr));
  TEST_ASSERT_EQUAL_UINT32(60, interval("1"));
  TEST_ASSERT_EQUAL_UINT32(3600, interval("3600"));
  TEST_ASSERT_EQUAL_UINT32(86400, interval("4294967295"));
}

void drawing_and_up_wakes_do_not_restart_deadline() {
  const uint64_t received = 100000000ULL;
  const uint64_t deadline = received + 3600ULL * 1000000;
  TEST_ASSERT_EQUAL_UINT64(3597000000ULL, sleepUs(deadline, received + 3000000));
  TEST_ASSERT_EQUAL_UINT64(1795000000ULL, sleepUs(deadline, received + 1805000000ULL));
  TEST_ASSERT_EQUAL_UINT64(60000000ULL, sleepUs(deadline, deadline));
  TEST_ASSERT_EQUAL_UINT64(60000000ULL, sleepUs(deadline, deadline + 10));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(frame_is_atomic_and_bounded);
  RUN_TEST(unsupported_and_encoded_frames_are_rejected);
  RUN_TEST(intervals_fall_back_or_clamp_without_overflow);
  RUN_TEST(drawing_and_up_wakes_do_not_restart_deadline);
  return UNITY_END();
}
