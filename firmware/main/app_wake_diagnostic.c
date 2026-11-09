/*
 * 空闲唤醒诊断：对比原始PCM和模型输入，统计命中/削波。
 * 采集期间不启动对话，时长/容量有界，不自动上传录音。
 */
#include "app_wake_diagnostic.h"
#include "app_wake.h"
#include "app_barge_diagnostic.h"
#include "app_memory_alloc.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CAPACITY (APP_WAKE_DIAGNOSTIC_SECONDS * APP_WAKE_DIAGNOSTIC_RATE)
#define RETENTION_US (300 * 1000000LL)
static SemaphoreHandle_t lock;
static int16_t *audio;
static app_wake_diagnostic_status_t status;
static unsigned sequence, pending_count;
static unsigned pending_clips;
static bool pending_model;
static int64_t started_us, expires_us;
static _Atomic bool active;
static _Atomic unsigned skipped;

void app_wake_diagnostic_init(void) {
  if (!lock) lock = xSemaphoreCreateMutex();
}
bool app_wake_diagnostic_active(void) { return atomic_load(&active); }
static void clear_locked(void) {
  atomic_store(&active, false);
  free(audio);
  audio = NULL;
  memset(&status, 0, sizeof(status));
  pending_count = 0;
  pending_model = false;
  expires_us = 0;
  atomic_store(&skipped, 0);
}
static void tick_locked(void) {
  int64_t now = esp_timer_get_time();
  if (status.active && (!app_wake_input_allowed() ||
                       now - started_us >= APP_WAKE_DIAGNOSTIC_SECONDS * 1000000LL ||
                       status.samples >= CAPACITY)) {
    status.active = false;
    status.reason = app_wake_input_allowed() ? 1 : 2;
    status.elapsed_ms = (unsigned)((now - started_us) / 1000);
    expires_us = now + RETENTION_US;
    atomic_store(&active, false);
  }
  if (audio && expires_us && now >= expires_us) clear_locked();
}
void app_wake_diagnostic_tick(void) {
  if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) return;
  tick_locked();
  xSemaphoreGive(lock);
}
esp_err_t app_wake_diagnostic_start(void) {
  if (!lock || !app_wake_input_allowed() || app_barge_diagnostic_busy()) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
  tick_locked();
  if (status.active) { xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE; }
  // Reuse the previous bounded buffer; normal wake/voice allocates no capture.
  if (!audio) audio = app_psram_malloc(CAPACITY * 2 * sizeof(int16_t));
  if (!audio) { xSemaphoreGive(lock); return ESP_ERR_NO_MEM; }
  if (!app_wake_input_allowed()) {
    clear_locked();
    xSemaphoreGive(lock);
    return ESP_ERR_INVALID_STATE;
  }
  memset(&status, 0, sizeof(status));
  if (!++sequence) ++sequence;
  status.id = sequence;
  status.active = true;
  started_us = esp_timer_get_time();
  expires_us = 0;
  pending_count = 0;
  pending_model = false;
  atomic_store(&skipped, 0);
  atomic_store(&active, true);
  xSemaphoreGive(lock);
  return ESP_OK;
}
esp_err_t app_wake_diagnostic_clear(void) {
  if (!lock) return ESP_OK;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
  clear_locked();
  xSemaphoreGive(lock);
  return ESP_OK;
}
bool app_wake_diagnostic_status(app_wake_diagnostic_status_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!lock) return true;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  tick_locked();
  *out = status;
  out->skipped_frames = atomic_load(&skipped);
  if (status.active)
    out->elapsed_ms = (unsigned)((esp_timer_get_time() - started_us) / 1000);
  xSemaphoreGive(lock);
  return true;
}
unsigned app_wake_diagnostic_raw(const int16_t *pcm, size_t count) {
  if (!atomic_load(&active)) return 0;
  if (xSemaphoreTake(lock, 0) != pdTRUE) {
    atomic_fetch_add(&skipped, 1);
    return 0;
  }
  tick_locked();
  unsigned id = 0;
  if (status.active && count && count <= CAPACITY - status.samples) {
    memcpy(audio + status.samples, pcm, count * sizeof(int16_t));
    pending_count = (unsigned)count;
    pending_model = false;
    id = status.id;
  }
  xSemaphoreGive(lock);
  return id;
}
void app_wake_diagnostic_model(unsigned id, const int16_t *pcm, size_t count,
                                unsigned clipped) {
  if (!id) return;
  if (xSemaphoreTake(lock, 0) != pdTRUE) {
    atomic_fetch_add(&skipped, 1);
    return;
  }
  if (id == status.id && status.active && pending_count == count &&
      count <= CAPACITY - status.samples) {
    memcpy(audio + CAPACITY + status.samples, pcm, count * sizeof(int16_t));
    for (size_t i = 0; i < count; ++i) {
      unsigned raw = (unsigned)abs((int)audio[status.samples + i]);
      unsigned model = (unsigned)abs((int)pcm[i]);
      if (raw > status.raw_peak) status.raw_peak = raw;
      if (model > status.model_peak) status.model_peak = model;
    }
    pending_clips = clipped;
    pending_model = true;
  }
  xSemaphoreGive(lock);
}
void app_wake_diagnostic_detected(unsigned id, bool hit) {
  if (!id) return;
  if (xSemaphoreTake(lock, 0) != pdTRUE) {
    atomic_fetch_add(&skipped, 1);
    return;
  }
  // Publish only complete pairs. A cleared/finished capture is immutable.
  if (id == status.id && status.active && pending_model) {
    status.samples += pending_count;
    status.hits += hit;
    status.clipped += pending_clips;
    pending_count = 0;
    pending_model = false;
    tick_locked();
  }
  xSemaphoreGive(lock);
}
size_t app_wake_diagnostic_read(unsigned id, bool model, size_t offset,
                                 void *out, size_t capacity) {
  if (!out || !lock || xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  tick_locked();
  size_t copied = 0, bytes = status.samples * sizeof(int16_t);
  if (id && id == status.id && !status.active && audio && offset < bytes) {
    copied = bytes - offset;
    if (copied > capacity) copied = capacity;
    memcpy(out, (const uint8_t *)(audio + (model ? CAPACITY : 0)) + offset, copied);
  }
  xSemaphoreGive(lock);
  return copied;
}
