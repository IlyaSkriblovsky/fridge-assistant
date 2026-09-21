#pragma once

#include <cstdlib>

// Host allocator shared by Recording and the e-paper driver. No simulation of PSRAM or FreeRTOS.
constexpr int MALLOC_CAP_SPIRAM = 1, MALLOC_CAP_8BIT = 2;
namespace testHeap {
inline bool failAllocation = false;
inline unsigned allocations = 0;
inline unsigned frees = 0;
}
inline void* heap_caps_malloc(size_t bytes, int) {
  if (testHeap::failAllocation) return nullptr;
  void* result = std::malloc(bytes);
  if (result) ++testHeap::allocations;
  return result;
}
inline void heap_caps_free(void* pointer) {
  if (pointer) ++testHeap::frees;
  std::free(pointer);
}
