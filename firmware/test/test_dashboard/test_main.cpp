#include <unity.h>
#include <vector>
#include <fstream>
#include <iterator>
#include <string>
#include <miniz.h>
#include <esp_heap_caps.h>
#include "dashboard_protocol.h"
#include "dashboard_png.h"

using namespace dashboardProtocol;
void setUp() { testHeap::failAllocation = false; }
void tearDown() {}

void frame_is_atomic_and_bounded() {
  std::vector<uint8_t> pixels(kMaxPngBytes + 1, 0xA5);
  std::vector<uint8_t> body(kMaxPngBytes, 0x80);
  Frame frame{pixels.data()};
  frame.header("Content-Type", "image/png");
  frame.header("dashboard-format", "png");
  TEST_ASSERT_TRUE(frame.append(body.data(), 1024));
  TEST_ASSERT_FALSE(frame.valid(200, kMaxPngBytes, true));
  TEST_ASSERT_TRUE(frame.append(body.data() + 1024, kMaxPngBytes - 1024));
  TEST_ASSERT_FALSE(frame.valid(200, kMaxPngBytes, false));
  TEST_ASSERT_FALSE(frame.valid(500, kMaxPngBytes, true));
  TEST_ASSERT_FALSE(frame.valid(200, kMaxPngBytes + 1, true));
  TEST_ASSERT_FALSE(frame.valid(200, 0, true));
  TEST_ASSERT_TRUE(frame.valid(200, kMaxPngBytes, true));
  TEST_ASSERT_EQUAL_MEMORY(body.data(), pixels.data(), kMaxPngBytes);
  TEST_ASSERT_FALSE(frame.append(body.data(), 1));
  TEST_ASSERT_EQUAL_HEX8(0xA5, pixels[kMaxPngBytes]);
  TEST_ASSERT_FALSE(frame.valid(200, kMaxPngBytes, true));
}

void unsupported_and_encoded_frames_are_rejected() {
  uint8_t byte = 0;
  for (const char* format : {"mono1-v1", "mono1-v2", ""}) {
    Frame frame{&byte};
    frame.header("Dashboard-Format", format);
    TEST_ASSERT_TRUE(frame.bad);
  }
  Frame frame{&byte};
  TEST_ASSERT_FALSE(frame.valid(200, kMaxPngBytes, true));
  frame.header("Content-Encoding", "gzip");
  TEST_ASSERT_TRUE(frame.bad);
  Frame duplicate{&byte};
  duplicate.header("Dashboard-Format", "png");
  duplicate.header("Dashboard-Format", "png");
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

std::vector<uint8_t> fixture(const char* name) {
  std::ifstream file(std::string("test/test_dashboard/fixtures/") + name, std::ios::binary);
  TEST_ASSERT_TRUE_MESSAGE(file.good(), name);
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void png_matches_server_and_all_filter_pixels() {
  for (const char* name : {"server", "filters"}) {
    auto png = fixture((std::string(name) + ".png").c_str());
    auto expected = fixture((std::string(name) + ".bitmap").c_str());
    std::vector<uint8_t> body(kMaxPngBytes);
    Frame response{body.data()};
    response.header("Content-Type", "image/png");
    response.header("Dashboard-Format", "png");
    response.header("Next-Update-After", "120");
    for (size_t offset = 0; offset < png.size(); offset += 37) {
      const size_t count = png.size() - offset < 37 ? png.size() - offset : 37;
      TEST_ASSERT_TRUE(response.append(png.data() + offset, count));
    }
    TEST_ASSERT_TRUE(response.valid(200, png.size(), true));
    TEST_ASSERT_EQUAL_UINT32(120, response.nextSeconds);
    std::vector<uint8_t> output(kFrameBytes + 2, 0xA5);
    TEST_ASSERT_TRUE(dashboardPng::decode(body.data(), response.size, output.data() + 1));
    TEST_ASSERT_EQUAL(kFrameBytes, expected.size());
    TEST_ASSERT_EQUAL_MEMORY(expected.data(), output.data() + 1, kFrameBytes);
    TEST_ASSERT_EQUAL_HEX8(0xA5, output.front());
    TEST_ASSERT_EQUAL_HEX8(0xA5, output.back());
  }
}

void crc(std::vector<uint8_t>& png, size_t offset) {
  const size_t count = uint32_t(png[offset]) << 24 | uint32_t(png[offset + 1]) << 16 |
                       uint32_t(png[offset + 2]) << 8 | png[offset + 3];
  uint32_t value = mz_crc32(0, png.data() + offset + 4, count + 4);
  for (int i = 3; i >= 0; --i) { png[offset + 8 + count + i] = value; value >>= 8; }
}

void png_rejects_unsupported_headers_and_chunks() {
  const auto original = fixture("server.png");
  std::vector<uint8_t> output(kFrameBytes);
  // Width, height, depth, colour type, compression, filter method, interlace.
  for (size_t field : {19, 23, 24, 25, 26, 27, 28}) {
    auto png = original;
    png[field] ^= 1;
    crc(png, 8);
    TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  }
  for (const char* type : {"tRNS", "PLTE", "IHDR", "abcd"}) {
    auto png = original;
    memcpy(png.data() + 37, type, 4); // First IDAT, with a valid CRC.
    crc(png, 33);
    TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  }
}

void png_rejects_corruption_truncation_and_extra_data() {
  const auto original = fixture("server.png");
  std::vector<uint8_t> output(kFrameBytes);
  for (size_t length = 0; length < original.size(); ++length) {
    auto png = original;
    TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), length, output.data()));
  }
  for (size_t offset : {size_t(0), size_t(29), size_t(45), original.size() - 1}) {
    auto png = original;
    png[offset] ^= 1;
    TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  }
  auto png = original;
  png.push_back(0);
  TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  png = original;
  png[33] = 0xFF; // Chunk length overflow.
  TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  png = original;
  png[41] = 0; // Bad zlib header, valid PNG CRC.
  crc(png, 33);
  TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  png = original;
  png[png.size() - 17] ^= 1; // Adler32 inside the single IDAT, valid PNG CRC.
  crc(png, 33);
  TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  png = original;
  const size_t idatBytes = png.size() - 57;
  png.insert(png.begin() + 41 + idatBytes, 0); // Trailing data inside IDAT.
  uint32_t extended = idatBytes + 1;
  for (int i = 3; i >= 0; --i) { png[33 + i] = extended; extended >>= 8; }
  crc(png, 33);
  TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  for (const char* name : {"short.png", "long.png", "bad-filter.png"}) {
    png = fixture(name);
    TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
  }
}

void png_cancellation_and_allocation_failure_release_scratch() {
  const auto original = fixture("server.png");
  std::vector<uint8_t> output(kFrameBytes);
  // Early parse, during inflation, and during unfiltering.
  for (int limit : {0, 10, 100}) {
    auto png = original;
    int remaining = limit;
    const unsigned allocated = testHeap::allocations, freed = testHeap::frees;
    TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data(),
        [](void* context) { return --*static_cast<int*>(context) < 0; }, &remaining));
    TEST_ASSERT_EQUAL(testHeap::allocations - allocated, testHeap::frees - freed);
  }
  auto png = original;
  testHeap::failAllocation = true;
  TEST_ASSERT_FALSE(dashboardPng::decode(png.data(), png.size(), output.data()));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(frame_is_atomic_and_bounded);
  RUN_TEST(unsupported_and_encoded_frames_are_rejected);
  RUN_TEST(intervals_fall_back_or_clamp_without_overflow);
  RUN_TEST(drawing_and_up_wakes_do_not_restart_deadline);
  RUN_TEST(png_matches_server_and_all_filter_pixels);
  RUN_TEST(png_rejects_unsupported_headers_and_chunks);
  RUN_TEST(png_rejects_corruption_truncation_and_extra_data);
  RUN_TEST(png_cancellation_and_allocation_failure_release_scratch);
  return UNITY_END();
}
