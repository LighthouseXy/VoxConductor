#pragma once

#include <string.h>
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifndef CONFIG_VOX_LOG_VERBOSE
#define CONFIG_VOX_LOG_VERBOSE 0
#endif

// Detail is INFO in opt-in builds: the normal SDK max level is only INFO.
// Preserve the global level and warning/error diagnostics.
#if CONFIG_VOX_LOG_VERBOSE
#define APP_LOG_DETAIL(...) ESP_LOGI(__VA_ARGS__)
#else
#define APP_LOG_DETAIL(...) ESP_LOGD(__VA_ARGS__)
#endif

// Console-only abbreviation. Persisted IDs and event correlation stay intact.
static inline const char *app_log_id(const char *id) {
  if (!id) return "-";
  const char *suffix = strrchr(id, '-');
  return suffix && suffix - id >= 8 ? suffix - 8 : id;
}
