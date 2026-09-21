#include <unity.h>
#include <esp_heap_caps.h>
#include <cstring>
#include "recording.h"

static Recording audio;
void setUp() {
  audio.end();
  testHeap::failAllocation = false;
  testHeap::allocations = testHeap::frees = 0;
}
void tearDown() { audio.end(); }

void header_is_streaming_mono_pcm_wav() {
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  const uint8_t expected[] = {
    'R','I','F','F', 255,255,255,255, 'W','A','V','E',
    'f','m','t',' ', 16,0,0,0, 1,0, 1,0,
    0x80,0x3e,0,0, 0,0x7d,0,0, 2,0,16,0,
    'd','a','t','a', 255,255,255,255
  };
  TEST_ASSERT_EQUAL_UINT(sizeof(expected), audio.wavBytes());
  TEST_ASSERT_EQUAL_MEMORY(expected, audio.wav(), sizeof(expected));
}

void samples_are_published_without_moving_or_overwriting_header() {
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  uint8_t header[44];
  std::memcpy(header, audio.wav(), sizeof(header));
  int16_t* samples = audio.writeHead();
  samples[0] = -1234;
  samples[1] = 32767;
  TEST_ASSERT_EQUAL_UINT(0, audio.recordedSamples());
  audio.commit(2);
  TEST_ASSERT_EQUAL_UINT(2, audio.recordedSamples());
  TEST_ASSERT_EQUAL_UINT(4, audio.recordedBytes());
  TEST_ASSERT_EQUAL_UINT(48, audio.wavBytes());
  TEST_ASSERT_EQUAL_PTR(samples + 2, audio.writeHead());
  int16_t published[2];
  std::memcpy(published, audio.wav() + 44, sizeof(published));
  TEST_ASSERT_EQUAL_INT16(-1234, published[0]);
  TEST_ASSERT_EQUAL_INT16(32767, published[1]);
  TEST_ASSERT_EQUAL_MEMORY(header, audio.wav(), sizeof(header));
  audio.commit(158);
  TEST_ASSERT_EQUAL_UINT(10, audio.recordedMs());
}

void final_chunk_and_excess_commit_stop_at_capacity() {
  TEST_ASSERT_TRUE(audio.begin(300, 1));
  TEST_ASSERT_EQUAL_UINT(256, audio.nextChunkSamples());
  audio.commit(256);
  TEST_ASSERT_FALSE(audio.full());
  TEST_ASSERT_EQUAL_UINT(44, audio.nextChunkSamples());
  audio.commit(100);
  TEST_ASSERT_TRUE(audio.full());
  TEST_ASSERT_EQUAL_UINT(300, audio.recordedSamples());
  TEST_ASSERT_EQUAL_UINT(1000, audio.recordedMs());
  TEST_ASSERT_EQUAL_UINT(0, audio.nextChunkSamples());
  audio.commit(UINT32_MAX);
  TEST_ASSERT_EQUAL_UINT(300, audio.recordedSamples());
}

void restart_reuses_buffer_and_resets_count() {
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  const uint8_t* buffer = audio.wav();
  audio.commit(200);
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  TEST_ASSERT_EQUAL_PTR(buffer, audio.wav());
  TEST_ASSERT_EQUAL_UINT(1, testHeap::allocations);
  TEST_ASSERT_EQUAL_UINT(0, testHeap::frees);
  TEST_ASSERT_EQUAL_UINT(0, audio.recordedSamples());
  TEST_ASSERT_EQUAL_UINT(44, audio.wavBytes());
  TEST_ASSERT_FALSE(audio.full());
}

void changed_rate_reallocates_and_rewrites_header() {
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  audio.commit(200);
  TEST_ASSERT_TRUE(audio.begin(8000, 2)); // Same capacity, different format.
  TEST_ASSERT_EQUAL_UINT(2, testHeap::allocations);
  TEST_ASSERT_EQUAL_UINT(1, testHeap::frees);
  const uint8_t rates[] = {0x40,0x1f,0,0, 0x80,0x3e,0,0};
  TEST_ASSERT_EQUAL_MEMORY(rates, audio.wav() + 24, sizeof(rates));
  TEST_ASSERT_EQUAL_UINT(0, audio.recordedSamples());
  audio.commit(8000);
  TEST_ASSERT_EQUAL_UINT(1000, audio.recordedMs());
}

void invalid_input_and_allocation_failure_are_recoverable() {
  TEST_ASSERT_FALSE(audio.begin(0, 1));
  TEST_ASSERT_FALSE(audio.begin(16000, 0));
  TEST_ASSERT_EQUAL_UINT(0, testHeap::allocations);
  testHeap::failAllocation = true;
  TEST_ASSERT_FALSE(audio.begin(16000, 1));
  TEST_ASSERT_NOT_EQUAL('\0', audio.lastError()[0]);
  TEST_ASSERT_NULL(audio.wav());
  TEST_ASSERT_NULL(audio.writeHead());
  TEST_ASSERT_EQUAL_UINT(0, audio.nextChunkSamples());
  audio.commit(100);
  TEST_ASSERT_EQUAL_UINT(0, audio.recordedSamples());
  testHeap::failAllocation = false;
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  TEST_ASSERT_EQUAL_STRING("", audio.lastError());
}

void end_is_idempotent_and_resets_recording() {
  TEST_ASSERT_TRUE(audio.begin(16000, 1));
  audio.commit(100);
  audio.end();
  audio.end();
  TEST_ASSERT_EQUAL_UINT(1, testHeap::frees);
  TEST_ASSERT_NULL(audio.wav());
  TEST_ASSERT_NULL(audio.writeHead());
  TEST_ASSERT_EQUAL_UINT(0, audio.recordedSamples());
  TEST_ASSERT_EQUAL_UINT(0, audio.recordedMs());
  TEST_ASSERT_EQUAL_UINT(0, audio.nextChunkSamples());
  TEST_ASSERT_FALSE(audio.full());
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(header_is_streaming_mono_pcm_wav);
  RUN_TEST(samples_are_published_without_moving_or_overwriting_header);
  RUN_TEST(final_chunk_and_excess_commit_stop_at_capacity);
  RUN_TEST(restart_reuses_buffer_and_resets_count);
  RUN_TEST(changed_rate_reallocates_and_rewrites_header);
  RUN_TEST(invalid_input_and_allocation_failure_are_recoverable);
  RUN_TEST(end_is_idempotent_and_resets_recording);
  return UNITY_END();
}
