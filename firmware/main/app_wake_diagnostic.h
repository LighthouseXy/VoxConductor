#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_WAKE_DIAGNOSTIC_SECONDS 12
#define APP_WAKE_DIAGNOSTIC_RATE 16000
typedef struct {
  unsigned id, samples, hits, clipped, skipped_frames;
  unsigned raw_peak, model_peak, elapsed_ms;
  bool active;
  // 0: none, 1: complete, 2: microphone owner changed.
  unsigned reason;
} app_wake_diagnostic_status_t;

void app_wake_diagnostic_init(void);
esp_err_t app_wake_diagnostic_start(void);
esp_err_t app_wake_diagnostic_clear(void);
bool app_wake_diagnostic_active(void);
void app_wake_diagnostic_tick(void);
bool app_wake_diagnostic_status(app_wake_diagnostic_status_t *out);
// Worker-only paired snapshots; a busy diagnostic lock never blocks WakeNet.
unsigned app_wake_diagnostic_raw(const int16_t *pcm, size_t count);
void app_wake_diagnostic_model(unsigned id, const int16_t *pcm, size_t count,
                                unsigned clipped);
void app_wake_diagnostic_detected(unsigned id, bool hit);
// Completed capture only. ID validation prevents mixing two recordings.
size_t app_wake_diagnostic_read(unsigned id, bool model, size_t offset,
                                 void *out, size_t capacity);
