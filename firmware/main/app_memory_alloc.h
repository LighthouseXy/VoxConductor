#pragma once
#include <stdlib.h>
#include <string.h>
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
// Large non-DMA objects use external RAM; host checks use the same algorithms.
static inline void *app_psram_malloc(size_t size) {
#ifdef ESP_PLATFORM
  return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
  return malloc(size);
#endif
}
static inline void *app_psram_zero(size_t size) {
  void *p = app_psram_malloc(size);
  if (p) memset(p, 0, size);
  return p;
}
static inline char *app_psram_strdup(const char *text) {
  if (!text) return NULL;
  size_t bytes = strlen(text) + 1;
  char *copy = app_psram_malloc(bytes);
  if (copy) memcpy(copy, text, bytes);
  return copy;
}
