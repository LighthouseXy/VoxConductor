/*
 * 本地WakeNet任务：消费有界PCM队列并检测配置的唤醒词。
 * 模型由工作任务创建/清理，避免跨任务销毁正在推理的对象。
 * 命中交给应用或声学协调器，此处不建立云端请求。
 */
#include "app_wake.h"
#include "app_acoustic.h"
#include "app_barge_diagnostic.h"
#include "app_wake_diagnostic.h"
#include "app_log.h"
#include "sdkconfig.h"
#if CONFIG_VOX_WAKE_WORD
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wn_models.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "model_path.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#ifndef CONFIG_VOX_WAKE_GAIN
#define CONFIG_VOX_WAKE_GAIN 1
#endif
#ifndef CONFIG_VOX_WAKE_THRESHOLD_PERCENT
#define CONFIG_VOX_WAKE_THRESHOLD_PERCENT 60
#endif
static _Atomic bool enabled, idle, barge, ready, reset;
static _Atomic unsigned generation;
static _Atomic unsigned submitted, dropped;
#define WAKE_QUEUE_FRAMES 16
#define WAKE_DEBOUNCE_US 800000
static unsigned producer_generation;
static const esp_wn_iface_t *wn;
static model_iface_data_t *model;
static srmodel_list_t *models;
static const char *model_name;
static QueueHandle_t frames;
typedef struct {
  unsigned generation;
  unsigned acoustic_generation;
  bool playback;
  _Alignas(16) int16_t pcm[];
} wake_frame_t;
static wake_frame_t *input, *work;
static size_t chunk, used;
static void (*wakeup)(void);
bool app_wake_available(void) { return atomic_load(&ready); }
bool app_wake_is_enabled(void) {
  return atomic_load(&enabled);
}
bool app_wake_input_allowed(void) {
  return atomic_load(&ready) && atomic_load(&enabled) && atomic_load(&idle);
}
void app_wake_allow(bool value) {
  if (atomic_exchange(&idle, value) != value) {
    atomic_fetch_add(&generation, 1);
    atomic_store(&reset, true);
  }
}
void app_wake_allow_barge(bool value) {
  if (atomic_exchange(&barge, value) != value) {
    atomic_fetch_add(&generation, 1);
    atomic_store(&reset, true);
  }
}
void app_wake_set_enabled(bool value) {
  if (atomic_exchange(&enabled, value) == value)
    return;
  atomic_fetch_add(&generation, 1);
  atomic_store(&reset, true);
  ESP_LOGI("wake", "Wake word %s", value ? "enabled" : "disabled");
}
static bool configure_model(void) {
  if (!wn->set_det_threshold || !wn->get_det_threshold ||
      !wn->set_det_threshold(model, CONFIG_VOX_WAKE_THRESHOLD_PERCENT / 100.0f, 1)) {
    ESP_LOGE("wake", "WakeNet threshold configuration failed");
    return false;
  }
  float threshold = wn->get_det_threshold(model, 1);
  if (!(threshold > 0.0f && threshold < 1.0f)) {
    ESP_LOGE("wake", "WakeNet returned an invalid threshold");
    return false;
  }
  int actual = (int)(threshold * 1000 + 0.5f);
  int expected = CONFIG_VOX_WAKE_THRESHOLD_PERCENT * 10;
  static int last_threshold = -1;
  if (actual != last_threshold) {
    ESP_LOGI("wake", "Wake sensitivity: threshold=%d/1000 (requested=%d/1000)",
             actual, expected);
    last_threshold = actual;
  } else {
    APP_LOG_DETAIL("wake", "Wake sensitivity: threshold=%d/1000 (requested=%d/1000)",
                   actual, expected);
  }
  // The bundled WakeNet9l setter derives a lower internal threshold (0.992x).
  // Its public getter reads that derived value, not the requested argument.
  // Setter success and a valid probability suffice; do not reject a good model.
  return true;
}
// Only this worker owns the model after startup. The bundled WakeNet9 clean()
// dereferences an invalid convolution queue before the first detect(); use its
// public destroy/create lifecycle rather than accessing private model fields.
static bool recreate_model(void) {
  atomic_store(&ready, false);
  wn->destroy(model);
  model = wn->create(model_name, DET_MODE_95);
  if (!model || wn->get_samp_chunksize(model) != (int)chunk ||
      wn->get_samp_rate(model) != 16000 || wn->get_channel_num(model) != 1 ||
      !configure_model()) {
    if (model)
      wn->destroy(model);
    model = NULL;
    atomic_store(&enabled, false);
    ESP_LOGE("wake", "WakeNet reset failed: internal=%u B, largest=%u B, PSRAM=%u B; button voice remains available",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // Keep the fixed queue/buffers: a producer may finish one in-flight feed.
    return false;
  }
  atomic_fetch_add(&generation, 1);
  xQueueReset(frames);
  atomic_store(&ready, true);
  return true;
}
static void worker(void *unused) {
  (void)unused;
  int64_t cooldown = 0;
  int64_t last_diag = 0;
  unsigned processed = 0, rebuilds = 0, hits = 0, suppressed = 0;
  unsigned peak_max = 0, mean_max = 0, infer_max_ms = 0;
  unsigned wake_peak_max = 0, clipped = 0;
  int32_t dc_q8 = 0;
  bool dc_initialized = false;
  bool model_used = false;
  while (true) {
    BaseType_t received = xQueueReceive(frames, work, pdMS_TO_TICKS(1000));
    int64_t now = esp_timer_get_time();
    app_wake_diagnostic_tick();
    app_barge_diagnostic_tick();
    // Bounded diagnostics also run when enabled but no microphone frames arrive.
    bool detailed = CONFIG_VOX_LOG_VERBOSE || app_wake_diagnostic_active();
    if (atomic_load(&enabled) &&
        (detailed || atomic_load(&idle) || !atomic_load(&ready)) &&
        now - last_diag >= (detailed ? 2000000 : 15000000)) {
      if (detailed) {
        ESP_LOGI("wake", "wake_diag: ready=%d allowed=%d queued=%u fed=%u processed=%u drop=%u rebuild=%u hit=%u debounce=%u pcm_peak=%u pcm_mean_max=%u infer_max=%ums gain=%u wn_peak=%u clip=%u dc=%d",
                 (int)atomic_load(&ready), (int)atomic_load(&idle),
                 (unsigned)uxQueueMessagesWaiting(frames),
                 atomic_load(&submitted), processed, atomic_load(&dropped),
                 rebuilds, hits, suppressed, peak_max, mean_max, infer_max_ms,
                 (unsigned)CONFIG_VOX_WAKE_GAIN, wake_peak_max, clipped,
                 (int)(dc_q8 / 256));
      } else {
        ESP_LOGI("wake", "wake_diag: ready=%d allowed=%d hit=%u processed=%u drop=%u pcm_peak=%u wn_peak=%u clip=%u",
                 (int)atomic_load(&ready), (int)atomic_load(&idle), hits,
                 processed, atomic_load(&dropped), peak_max, wake_peak_max, clipped);
      }
      last_diag = now;
      peak_max = mean_max = infer_max_ms = wake_peak_max = clipped = 0;
    }
    if (atomic_exchange(&reset, false)) {
      dc_q8 = 0;
      dc_initialized = false;
      // A fresh instance needs no reset before its first detect().
      if (model_used && !recreate_model())
        continue;
      if (model_used)
        ++rebuilds;
      model_used = false;
      xQueueReset(frames);
      continue;
    }
    if (received != pdTRUE)
      continue;
    if (!app_wake_available() || !app_wake_is_enabled() ||
        !(work->playback ? atomic_load(&barge) : atomic_load(&idle)) ||
        work->generation != atomic_load(&generation)) {
      if (work->playback) app_barge_diagnostic_add(work->acoustic_generation, BD_STALE, 1);
      continue;
    }
    unsigned detecting_generation = atomic_load(&generation);
    unsigned diagnostic_id = work->playback ? 0 : app_wake_diagnostic_raw(work->pcm, chunk);
    unsigned clips_before = clipped;
    unsigned sum = 0;
    for (size_t i = 0; i < chunk; ++i) {
      int32_t raw = work->pcm[i];
      unsigned magnitude = (unsigned)abs((int)raw);
      sum += magnitude;
      if (magnitude > peak_max)
        peak_max = magnitude;
      // A continuous ~10 Hz DC blocker, not a separate mean per DMA block.
      // Q8 state preserves small signals. This modifies only the worker copy:
      // normal recording and the acoustic interruption path keep their input.
      if (!dc_initialized) {
        dc_q8 = raw * 256;
        dc_initialized = true;
      }
      dc_q8 += (raw * 256 - dc_q8) / 256;
      int32_t scaled = (raw - dc_q8 / 256) * CONFIG_VOX_WAKE_GAIN;
      if (scaled > INT16_MAX) {
        scaled = INT16_MAX;
        ++clipped;
      } else if (scaled < INT16_MIN) {
        scaled = INT16_MIN;
        ++clipped;
      }
      work->pcm[i] = (int16_t)scaled;
      magnitude = (unsigned)abs((int)scaled);
      if (magnitude > wake_peak_max)
        wake_peak_max = magnitude;
    }
    if (sum / chunk > mean_max)
      mean_max = (unsigned)(sum / chunk);
    app_wake_diagnostic_model(diagnostic_id, work->pcm, chunk, clipped - clips_before);
    model_used = true;
    int64_t detect_started = esp_timer_get_time();
    int detected = wn->detect(model, work->pcm);
    if (work->playback) {
      app_barge_diagnostic_pcm(work->acoustic_generation, 3, work->pcm, chunk);
      app_barge_diagnostic_add(work->acoustic_generation, BD_PROCESSED, 1);
      app_barge_diagnostic_add(work->acoustic_generation, BD_HIT, detected > 0 ? 1 : 0);
      app_barge_diagnostic_clips(work->acoustic_generation, clipped - clips_before);
      if (detected > 0 && (detecting_generation != atomic_load(&generation) ||
          atomic_load(&reset) || !atomic_load(&enabled) || !atomic_load(&barge)))
        app_barge_diagnostic_add(work->acoustic_generation, BD_REJECT_STATE, 1);
    }
    app_wake_diagnostic_detected(diagnostic_id, detected > 0);
    ++processed;
    unsigned elapsed_ms = (unsigned)((esp_timer_get_time() - detect_started) / 1000);
    if (elapsed_ms > infer_max_ms)
      infer_max_ms = elapsed_ms;
    if (detected > 0 && detecting_generation == atomic_load(&generation) &&
        !atomic_load(&reset) && atomic_load(&enabled) &&
        (work->playback ? atomic_load(&barge) : atomic_load(&idle))) {
      ++hits;
      // An explicitly requested calibration must not open cloud recording.
      if (diagnostic_id || app_wake_diagnostic_active()) {
        ESP_LOGI("wake", "Diagnostic detected: %s; conversation not started", APP_WAKE_PHRASE);
        continue;
      }
      // Debounce duplicate notifications, not the audio stream. Throwing away
      // three seconds of frames after an ignored wake loses repeated attempts.
      now = esp_timer_get_time();
      if (now < cooldown) {
        ++suppressed;
        if (work->playback) app_barge_diagnostic_add(work->acoustic_generation, BD_DEBOUNCE, 1);
        continue;
      }
      cooldown = now + WAKE_DEBOUNCE_US;
      atomic_fetch_add(&generation, 1);
      atomic_store(&reset, true);
      if (work->playback) {
        if (app_acoustic_wake_detected(work->acoustic_generation))
          ESP_LOGI("wake", "小冰小冰 detected during playback; requesting handoff");
        continue;
      }
      ESP_LOGI("wake", "Wake word detected: %s (id=%d)", APP_WAKE_PHRASE, detected);
      if (wakeup)
        wakeup();
    }
  }
}
void app_wake_init(void (*callback)(void)) {
  wakeup = callback;
  app_wake_diagnostic_init();
  models = esp_srmodel_init("model");
  // A wake word is trained into the model, not configured by a text string.
  // Never silently activate an unrelated bundled wake word.
  char *name = NULL;
  if (models && CONFIG_VOX_WAKE_MODEL_NAME[0])
    for (int i = 0; i < models->num; ++i)
      if (!strcmp(models->model_name[i], CONFIG_VOX_WAKE_MODEL_NAME)) {
        name = models->model_name[i];
        break;
      }
  wn = name ? esp_wn_handle_from_name(name) : NULL;
  model_name = name;
  model = wn ? wn->create(name, DET_MODE_95) : NULL;
  if (model)
    chunk = wn->get_samp_chunksize(model);
  if (!model || !chunk || chunk > 1024 || wn->get_samp_rate(model) != 16000 ||
      wn->get_channel_num(model) != 1 || !configure_model())
    goto fail;
  size_t frame_bytes = sizeof(wake_frame_t) + chunk * sizeof(int16_t);
  input = heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  work = heap_caps_aligned_alloc(16, frame_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  frames = xQueueCreateWithCaps(WAKE_QUEUE_FRAMES, frame_bytes,
                                MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (!input || !work || !frames ||
      xTaskCreatePinnedToCore(worker, "wake_word", 6144, NULL, 4, NULL, 1) != pdPASS)
    goto fail;
  atomic_store(&ready, true);
  ESP_LOGI("wake", "本地唤醒模型就绪：%s / %s；需在设置中开启，不上传空闲音频",
           name, esp_srmodel_get_wake_words(models, name));
  ESP_LOGI("wake", "Wake input: 16000 Hz mono, chunk=%u samples, PSRAM queue=%u frames (%u ms), DC blocker, gain=%ux, threshold=%d/1000",
           (unsigned)chunk, (unsigned)WAKE_QUEUE_FRAMES,
           (unsigned)(WAKE_QUEUE_FRAMES * chunk * 1000 / 16000),
           (unsigned)CONFIG_VOX_WAKE_GAIN,
           wn->get_det_threshold ? (int)(wn->get_det_threshold(model, 1) * 1000) : -1);
  return;
fail:
  if (frames)
    vQueueDeleteWithCaps(frames);
  frames = NULL;
  free(input);
  free(work);
  input = work = NULL;
  if (model)
    wn->destroy(model);
  model = NULL;
  if (models)
    esp_srmodel_deinit(models);
  models = NULL;
  ESP_LOGW("wake", "%s专用模型未就绪，按键语音继续可用", APP_WAKE_PHRASE);
}
static void feed_pcm(const int16_t *pcm, size_t count, bool playback, unsigned epoch) {
  // One microphone producer. Drop a newest frame on overload, never rebuild
  // the model here: repeated overflow/reset would prevent continuous detection.
  // An ineligible raw feed must not erase a partial echo-cleaned frame.
  if (!app_wake_available() || !app_wake_is_enabled() ||
      !(playback ? atomic_load(&barge) : atomic_load(&idle))) {
    if (playback) app_barge_diagnostic_add(epoch, BD_DISABLED, 1);
    return;
  }
  unsigned current = atomic_load(&generation);
  if (producer_generation != current) {
    producer_generation = current;
    used = 0;
  }
  for (size_t i = 0; i < count; i++) {
    input->pcm[used++] = pcm[i];
    if (used == chunk) {
      used = 0;
      input->generation = producer_generation;
      input->playback = playback;
      input->acoustic_generation = epoch;
      if (xQueueSend(frames, input, 0) != pdTRUE) {
        atomic_fetch_add(&dropped, 1);
        if (playback) app_barge_diagnostic_add(epoch, BD_DROP, 1);
      } else {
        atomic_fetch_add(&submitted, 1);
        if (playback) app_barge_diagnostic_add(epoch, BD_QUEUED, 1);
      }
    }
  }
}
void app_wake_feed(const int16_t *pcm, size_t count) {
  feed_pcm(pcm, count, false, 0);
}
void app_wake_feed_barge(const int16_t *pcm, size_t count, unsigned epoch) {
  feed_pcm(pcm, count, true, epoch);
}
#else
void app_wake_init(void (*callback)(void)) { (void)callback; }
void app_wake_feed(const int16_t *pcm, size_t count) {
  (void)pcm;
  (void)count;
}
void app_wake_allow(bool idle) { (void)idle; }
void app_wake_allow_barge(bool active) { (void)active; }
void app_wake_feed_barge(const int16_t *pcm, size_t count, unsigned epoch) {
  (void)pcm; (void)count; (void)epoch;
}
void app_wake_set_enabled(bool enabled) { (void)enabled; }
bool app_wake_is_enabled(void) { return false; }
bool app_wake_available(void) { return false; }
bool app_wake_input_allowed(void) { return false; }
#endif
