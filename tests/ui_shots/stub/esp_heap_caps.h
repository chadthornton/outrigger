#pragma once
#include <stdlib.h>
#define MALLOC_CAP_DMA 1
#define MALLOC_CAP_INTERNAL 2
static inline void *heap_caps_malloc(size_t n, int) { return malloc(n); }
