#pragma once
#include <cstddef>
constexpr int MALLOC_CAP_INTERNAL = 1, MALLOC_CAP_8BIT = 2;
constexpr int MALLOC_CAP_DMA = 4;
inline size_t hostLargestHeap = 65536;
inline size_t heap_caps_get_free_size(int) { return hostLargestHeap; }
inline size_t heap_caps_get_largest_free_block(int) { return hostLargestHeap; }