/*
 * 播报打断诊断：原始输入、参考、AEC/模型输入的有界片段。
 * 下载核对采集ID/长度，释放不能与采集/下载竞争。
 */
#include "app_barge_diagnostic.h"
#include "app_wake.h"
#include "app_wake_diagnostic.h"
#include "app_memory_alloc.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "sdkconfig.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

#define CAP (BARGE_DIAG_RATE * BARGE_DIAG_SECONDS)
#define BYTES (CAP * BARGE_DIAG_TRACKS * sizeof(int16_t))
#define RETENTION_US (300 * 1000000LL)
#define WAIT_US (120 * 1000000LL)
static SemaphoreHandle_t lock;
static int16_t *audio;
static app_barge_diagnostic_status_t status;
static unsigned sequence;
static int64_t armed_us, started_us, expires_us;
static _Atomic bool busy;
static _Atomic unsigned active_epoch, counters[BD_COUNT], clips, skipped;

void app_barge_diagnostic_init(void) {
  if (!lock) lock = xSemaphoreCreateMutex();
}
bool app_barge_diagnostic_busy(void) { return atomic_load(&busy); }
static void clear_locked(void) {
  atomic_store(&active_epoch, 0);
  atomic_store(&busy, false);
  free(audio); audio = NULL;
  memset(&status, 0, sizeof(status));
  expires_us = 0;
}
static void snapshot_locked(void) {
  for (unsigned i = 0; i < BD_COUNT; ++i) status.counts[i] = atomic_load(&counters[i]);
  status.clipped = atomic_load(&clips);
  status.skipped = atomic_load(&skipped);
}
static void finish_locked(unsigned reason) {
  if (status.state != 1 && status.state != 2) return;
  atomic_store(&active_epoch, 0);
  status.elapsed_ms = status.state == 2 ? (unsigned)((esp_timer_get_time() - started_us) / 1000) : 0;
  status.state = 3; status.reason = reason;
  snapshot_locked();
  atomic_store(&busy, false);
  expires_us = esp_timer_get_time() + RETENTION_US;
  ESP_LOGI("barge_diag", "id=%u end=%u mic_reads=%u mic_frames=%u aec=%u queued=%u processed=%u hit=%u accepted=%u drop=%u stale=%u disabled=%u debounce=%u reject_epoch=%u reject_state=%u duplicate=%u handoff_race=%u ref_missing_samples=%u ref_overflow=%u clips=%u capture_skips=%u",
      status.id, reason, status.counts[BD_INPUT], status.counts[BD_MIC], status.counts[BD_AEC],
      status.counts[BD_QUEUED], status.counts[BD_PROCESSED], status.counts[BD_HIT],
      status.counts[BD_ACCEPT], status.counts[BD_DROP], status.counts[BD_STALE],
      status.counts[BD_DISABLED], status.counts[BD_DEBOUNCE],
      status.counts[BD_REJECT_EPOCH], status.counts[BD_REJECT_STATE], status.counts[BD_DUPLICATE],
      status.counts[BD_HANDOFF_RACE], status.counts[BD_REF_MISSING], status.counts[BD_REF_OVERFLOW],
      status.clipped, status.skipped);
}
static void tick_locked(void) {
  int64_t now = esp_timer_get_time();
  if (status.state == 1 && now - armed_us >= WAIT_US) finish_locked(BD_WAIT_TIMEOUT);
  if (status.state == 2 && now - started_us >= BARGE_DIAG_SECONDS * 1000000LL)
    finish_locked(BD_DURATION);
  if (audio && expires_us && now >= expires_us) clear_locked();
}
void app_barge_diagnostic_tick(void) {
  if (!lock || xSemaphoreTake(lock, 0) != pdTRUE) return;
  tick_locked(); xSemaphoreGive(lock);
}
esp_err_t app_barge_diagnostic_start(void) {
#if !CONFIG_VOX_ACOUSTIC_BARGE_IN
  return ESP_ERR_NOT_SUPPORTED;
#endif
  if (!lock || !app_wake_input_allowed() || app_wake_diagnostic_active()) return ESP_ERR_INVALID_STATE;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
  tick_locked();
  if (atomic_load(&busy)) { xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE; }
  // Allocate only with explicit consent, before voice TLS/DSP allocation.
  unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (!audio && heap_caps_get_free_size(caps) >= BYTES + 2 * 1024 * 1024)
    audio = app_psram_malloc(BYTES);
  if (!audio) { xSemaphoreGive(lock); return ESP_ERR_NO_MEM; }
  if (!app_wake_input_allowed() || app_wake_diagnostic_active()) {
    clear_locked(); xSemaphoreGive(lock); return ESP_ERR_INVALID_STATE;
  }
  memset(&status, 0, sizeof(status));
  for (unsigned i = 0; i < BD_COUNT; ++i) atomic_store(&counters[i], 0);
  atomic_store(&clips, 0); atomic_store(&skipped, 0);
  if (!++sequence) ++sequence;
  status.id = sequence; status.state = 1;
  armed_us = esp_timer_get_time(); expires_us = 0;
  atomic_store(&busy, true);
  xSemaphoreGive(lock); return ESP_OK;
}
esp_err_t app_barge_diagnostic_clear(void) {
  if (!lock) return ESP_OK;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return ESP_ERR_TIMEOUT;
  clear_locked(); xSemaphoreGive(lock); return ESP_OK;
}
bool app_barge_diagnostic_status(app_barge_diagnostic_status_t *out) {
  if (!out) return false;
  memset(out, 0, sizeof(*out));
  if (!lock) return true;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return false;
  tick_locked();
  if (status.state == 2) {
    snapshot_locked(); status.elapsed_ms = (unsigned)((esp_timer_get_time() - started_us) / 1000);
  }
  *out = status; xSemaphoreGive(lock); return true;
}
void app_barge_diagnostic_begin(unsigned epoch) {
  if (!epoch || !atomic_load(&busy) || !lock) return;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
  tick_locked();
  if (status.state == 1) {
    status.state = 2; started_us = esp_timer_get_time();
    atomic_store(&active_epoch, epoch);
    ESP_LOGI("barge_diag", "id=%u recording next answer for at most %us", status.id, BARGE_DIAG_SECONDS);
  }
  xSemaphoreGive(lock);
}
void app_barge_diagnostic_end(unsigned epoch, barge_diag_reason_t reason) {
  if (!atomic_load(&busy) || !lock) return;
  if (xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return;
  if ((epoch && epoch == atomic_load(&active_epoch)) || (!epoch && status.state == 1)) finish_locked(reason);
  xSemaphoreGive(lock);
}
void app_barge_diagnostic_add(unsigned epoch, barge_diag_counter_t counter, unsigned n) {
  if (epoch && epoch == atomic_load(&active_epoch) && counter < BD_COUNT)
    atomic_fetch_add(&counters[counter], n);
}
void app_barge_diagnostic_clips(unsigned epoch, unsigned n) {
  if (epoch && epoch == atomic_load(&active_epoch)) atomic_fetch_add(&clips, n);
}
void app_barge_diagnostic_input(void) {
  app_barge_diagnostic_add(atomic_load(&active_epoch), BD_INPUT, 1);
}
void app_barge_diagnostic_pcm(unsigned epoch, unsigned track, const int16_t *pcm, size_t count) {
  if (!epoch || epoch != atomic_load(&active_epoch) || track >= BARGE_DIAG_TRACKS || !pcm) return;
  // Real-time producers never wait for the web reader; report missing capture blocks.
  if (xSemaphoreTake(lock, 0) != pdTRUE) { atomic_fetch_add(&skipped, 1); return; }
  if (epoch == atomic_load(&active_epoch) && status.state == 2) {
    size_t n = CAP - status.samples[track];
    if (n > count) n = count;
    memcpy(audio + track * CAP + status.samples[track], pcm, n * sizeof(*pcm));
    status.samples[track] += n;
    for (size_t i = 0; i < n; ++i) {
      unsigned peak = (unsigned)abs((int)pcm[i]);
      if (peak > status.peaks[track]) status.peaks[track] = peak;
    }
  }
  xSemaphoreGive(lock);
}
size_t app_barge_diagnostic_read(unsigned id, unsigned track, size_t offset, void *out, size_t capacity) {
  if (!out || track >= BARGE_DIAG_TRACKS || !lock || xSemaphoreTake(lock, pdMS_TO_TICKS(20)) != pdTRUE) return 0;
  tick_locked(); size_t n = 0;
  size_t bytes = status.samples[track] * sizeof(int16_t);
  if (audio && id && id == status.id && status.state == 3 && offset < bytes) {
    n = bytes - offset; if (n > capacity) n = capacity;
    memcpy(out, (uint8_t *)(audio + track * CAP) + offset, n);
  }
  xSemaphoreGive(lock); return n;
}
