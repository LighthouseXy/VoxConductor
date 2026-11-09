#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static inline void *heap_caps_malloc(size_t size, unsigned caps) { (void)caps; return malloc(size); }
static inline size_t heap_caps_get_free_size(unsigned caps) { (void)caps; return 1024 * 1024; }
static inline size_t heap_caps_get_largest_free_block(unsigned caps) { (void)caps; return 1024 * 1024; }
