#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BARGE_DIAG_RATE 16000
#define BARGE_DIAG_SECONDS 12
#define BARGE_DIAG_TRACKS 4
// Tracks: raw microphone, aligned speaker reference, AEC output, WakeNet input.
// First three use the acoustic clock; model track excludes skipped queue frames.
typedef enum {
  BD_INPUT, BD_MIC, BD_AEC, BD_QUEUED, BD_DROP, BD_PROCESSED, BD_HIT,
  BD_STALE, BD_DEBOUNCE, BD_DISABLED, BD_REF_MISSING, BD_REF_OVERFLOW,
  BD_REQUEST, BD_REJECT_EPOCH, BD_REJECT_STATE, BD_DUPLICATE,
  BD_ACCEPT, BD_HANDOFF_RACE, BD_COUNT
} barge_diag_counter_t;
typedef enum { BD_DURATION = 1, BD_PLAY_END, BD_WAIT_TIMEOUT, BD_UNAVAILABLE } barge_diag_reason_t;
typedef struct {
  unsigned id, state, reason, elapsed_ms; // 0 empty, 1 waiting, 2 recording, 3 complete
  unsigned samples[BARGE_DIAG_TRACKS], peaks[BARGE_DIAG_TRACKS];
  unsigned counts[BD_COUNT], clipped, skipped;
} app_barge_diagnostic_status_t;
void app_barge_diagnostic_init(void);
bool app_barge_diagnostic_busy(void);
esp_err_t app_barge_diagnostic_start(void);
esp_err_t app_barge_diagnostic_clear(void);
void app_barge_diagnostic_tick(void);
bool app_barge_diagnostic_status(app_barge_diagnostic_status_t *out);
void app_barge_diagnostic_begin(unsigned epoch);
void app_barge_diagnostic_end(unsigned epoch, barge_diag_reason_t reason);
void app_barge_diagnostic_add(unsigned epoch, barge_diag_counter_t counter, unsigned n);
void app_barge_diagnostic_input(void);
void app_barge_diagnostic_pcm(unsigned epoch, unsigned track, const int16_t *pcm, size_t count);
void app_barge_diagnostic_clips(unsigned epoch, unsigned n);
size_t app_barge_diagnostic_read(unsigned id, unsigned track, size_t offset, void *out, size_t capacity);
