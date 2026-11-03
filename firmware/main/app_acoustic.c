/*
 * 播报期间的声学前端/交接，参考来自实际音量缩放后的输出。
 * WakeNet命中只请求交接，不在模型回调创建对话；播放代次隔离过期命中。
 * 显式结束使待续问失效，已捕获问题由下一轮一次性消费。
 */
#include "app_acoustic.h"
#include "app_audio.h"
#include "app_barge_diagnostic.h"
#include "sdkconfig.h"
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "app_cloud.h"
#include "app_log.h"
#include "app_wake.h"
#include "esp_aec.h"
#include "esp_vad.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"

#define RATE 16000
#define REF_CAP 8192
#define PRE_CAP RATE
#define CAPTURE_CAP (20 * RATE)
#define VAD_SAMPLES 320
#define SILENCE_SAMPLES (RATE * CONFIG_VOX_AUTO_END_SILENCE_MS / 1000)
static aec_handle_t *aec;
static vad_handle_t vad;
static int16_t *buffers, *mic, *ref, *clean, *rings, *reference, *preroll, *pending_pcm;
static int16_t vad_frame[VAD_SAMPLES];
static size_t frame, used, ref_read, ref_write, pre_pos, pre_count;
static size_t captured, pending_silence, vad_used;
static int32_t vad_dc_q8;
static unsigned seen_generation;
static bool capture_finished;
static bool pending_question;
static size_t pending_voice_run;
static int64_t warmup_until, echo_tail_until;
static portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
static _Atomic bool ready, listening, playing, pending, blocked;
static _Atomic unsigned generation, requested;

// Only the microphone reader writes PCM/counters. take() holds its I2S mutex.
void app_acoustic_init(void) {
  app_barge_diagnostic_init();
  ESP_LOGI("acoustic", "小冰小冰 interruption configured; echo processing loads after ASR");
}
static bool create_dsp(void) {
  if (atomic_load(&ready)) return true;
  unsigned caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
  if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) < 48 * 1024 ||
      heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 8192 ||
      heap_caps_get_free_size(caps) < 1024 * 1024 ||
      heap_caps_get_largest_free_block(caps) < CAPTURE_CAP * sizeof(int16_t)) {
    ESP_LOGW("acoustic", "小冰小冰 interruption unavailable: internal=%u largest=%u PSRAM=%u; use button",
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned)heap_caps_get_free_size(caps));
    return false;
  }
  aec_config_t config = {.mic_num = 1, .ref_num = 1, .out_num = 1,
      .filter_length = 4, .sample_rate = RATE, .caps = caps,
      .mode = AEC_MODE_FD_LOW_COST, .nlp_level = AEC_NLP_LEVEL_AGGR};
  aec = aec_create_from_config(&config);
  frame = aec ? (size_t)aec_get_chunksize(aec) : 0;
  if (!frame || frame > 1024 || frame % 8) goto failed;
  buffers = heap_caps_aligned_alloc(16, frame * 3 * sizeof(int16_t), caps);
  rings = heap_caps_malloc((REF_CAP + PRE_CAP) * sizeof(int16_t), caps);
  pending_pcm = heap_caps_malloc(CAPTURE_CAP * sizeof(int16_t), caps);
  if (!buffers || !rings || !pending_pcm ||
      heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 4096) goto failed;
  // VAD ends an already accepted question; it never triggers interruption.
  vad = vad_create_with_param((vad_mode_t)CONFIG_VOX_AUTO_END_VAD_MODE, RATE, 20, 80, 20);
  if (!vad) goto failed;
  mic = buffers; ref = buffers + frame; clean = buffers + frame * 2;
  reference = rings; preroll = rings + REF_CAP;
  atomic_store_explicit(&ready, true, memory_order_release);
  return true;
failed:
  if (aec) aec_destroy(aec);
  if (vad) vad_destroy(vad);
  free(buffers); free(rings); free(pending_pcm);
  aec = NULL; vad = NULL; buffers = rings = pending_pcm = NULL;
  ESP_LOGW("acoustic", "Echo processing allocation failed; use button");
  return false;
}
bool app_acoustic_prepare(void) {
  return app_wake_is_enabled() && !app_cloud_is_cancelled() && create_dsp();
}
bool app_acoustic_listen(bool active) {
  // DSP allocation is completed before the LLM/TTS request, never mid-playback.
  if (active && (!atomic_load(&ready) || !app_wake_is_enabled() ||
                 !app_wake_available() || app_cloud_is_cancelled())) return false;
  portENTER_CRITICAL(&lock);
  if (active && app_cloud_is_cancelled()) {
    portEXIT_CRITICAL(&lock);
    return false;
  }
  if (atomic_exchange(&listening, active) != active) {
    atomic_store(&requested, 0);
    if (!atomic_load(&pending)) atomic_fetch_add(&generation, 1);
    if (active) {
      atomic_store(&playing, false);
      atomic_store(&blocked, false);
      ref_read = ref_write = 0;
      echo_tail_until = 0;
    }
  }
  portEXIT_CRITICAL(&lock);
  if (!active) app_wake_allow_barge(false);
  return true;
}
void app_acoustic_discard(void) {
  portENTER_CRITICAL(&lock);
  unsigned old_epoch = atomic_load(&generation);
  atomic_store(&listening, false);
  atomic_store(&playing, false);
  atomic_store(&pending, false);
  atomic_store(&requested, 0);
  atomic_fetch_add(&generation, 1);
  echo_tail_until = 0;
  portEXIT_CRITICAL(&lock);
  app_barge_diagnostic_end(old_epoch, BD_PLAY_END);
  app_wake_allow_barge(false);
}
void app_acoustic_playing(bool active) {
  if (!atomic_load_explicit(&ready, memory_order_acquire)) return;
  bool arm = false;
  portENTER_CRITICAL(&lock);
  unsigned epoch = atomic_load(&generation);
  if (active && atomic_load(&listening) && !atomic_load(&pending) &&
      !app_cloud_is_cancelled() && !atomic_exchange(&playing, true)) {
    ref_read = 0;
    ref_write = CONFIG_VOX_AEC_DELAY_MS * (RATE / 1000);
    ref_write += app_audio_reference_extra_samples();
    memset(reference, 0, ref_write * sizeof(*reference));
    warmup_until = esp_timer_get_time() + 160000;
    arm = true;
  } else if (!active && atomic_exchange(&playing, false)) {
    echo_tail_until = esp_timer_get_time() + 200000;
  }
  portEXIT_CRITICAL(&lock);
  if (arm) {
    app_barge_diagnostic_begin(epoch);
    app_wake_allow_barge(true);
    ESP_LOGI("acoustic", "小冰小冰 interruption armed");
  } else if (!active) {
    app_wake_allow_barge(false);
    app_barge_diagnostic_end(epoch, BD_PLAY_END);
  }
}
void app_acoustic_reference(const int16_t *pcm, size_t count) {
  if (!pcm || !count || !atomic_load(&ready) || !atomic_load(&playing)) return;
  bool fault = false;
  portENTER_CRITICAL(&lock);
  if (atomic_load(&playing)) {
    if (count > REF_CAP || ref_write - ref_read + count > REF_CAP) {
      ref_read = ref_write = 0;
      atomic_store(&requested, 0);
      fault = !atomic_exchange(&blocked, true);
    }
    if (count <= REF_CAP)
      for (size_t i = 0; i < count; ++i) reference[ref_write++ % REF_CAP] = pcm[i];
  }
  portEXIT_CRITICAL(&lock);
  if (fault) {
    app_barge_diagnostic_add(atomic_load(&generation), BD_REF_OVERFLOW, 1);
    app_wake_allow_barge(false);
    ESP_LOGW("acoustic", "Reference discontinuity; 小冰小冰 interruption disabled for this answer, use button");
  }
}
bool app_acoustic_wake_detected(unsigned epoch) {
  bool accepted = false;
  barge_diag_counter_t outcome = BD_REJECT_STATE;
  portENTER_CRITICAL(&lock);
  if (epoch != atomic_load(&generation)) outcome = BD_REJECT_EPOCH;
  if (epoch == atomic_load(&generation) && atomic_load(&listening) &&
      atomic_load(&playing) && !atomic_load(&blocked) &&
      !atomic_load(&pending) && !app_cloud_is_cancelled() && app_wake_is_enabled()) {
    unsigned empty = 0;
    accepted = atomic_compare_exchange_strong(&requested, &empty, epoch);
    outcome = accepted ? BD_REQUEST : BD_DUPLICATE;
  }
  portEXIT_CRITICAL(&lock);
  app_barge_diagnostic_add(epoch, outcome, 1);
  return accepted;
}
static void accept_requested_wake(void) {
  unsigned epoch = atomic_exchange(&requested, 0);
  if (!epoch) return;
  if (epoch != seen_generation) {
    app_barge_diagnostic_add(epoch, BD_HANDOFF_RACE, 1);
    return;
  }
  // Snapshot outside the spinlock; only this microphone owner can change rings.
  captured = pre_count;
  pending_question = false;
  pending_voice_run = 0;
  for (size_t i = 0; i < captured; ++i)
    pending_pcm[i] = preroll[(pre_pos + PRE_CAP - pre_count + i) % PRE_CAP];
  pending_silence = vad_used = 0; vad_dc_q8 = 0; capture_finished = false;
  vad_reset_trigger(vad);
  bool accepted = false;
  portENTER_CRITICAL(&lock);
  if (epoch == atomic_load(&generation) && atomic_load(&listening) &&
      atomic_load(&playing) && !atomic_load(&blocked) &&
      !atomic_load(&pending) && app_wake_is_enabled() && app_cloud_barge_in()) {
    atomic_store_explicit(&pending, true, memory_order_release);
    accepted = true;
  }
  portEXIT_CRITICAL(&lock);
  app_barge_diagnostic_add(epoch, accepted ? BD_ACCEPT : BD_HANDOFF_RACE, 1);
  if (accepted) {
    app_wake_allow_barge(false);
    ESP_LOGI("acoustic", "小冰小冰 interruption accepted; old answer cancelled, pre-roll=%u ms",
             (unsigned)(captured * 1000 / RATE));
  }
}
void app_acoustic_feed(const int16_t *pcm, size_t count) {
  if (!pcm || !count || !atomic_load_explicit(&ready, memory_order_acquire)) return;
  if ((!atomic_load(&listening) || !atomic_load(&playing)) && !atomic_load(&pending)) return;
  unsigned current = atomic_load(&generation);
  if (current != seen_generation) {
    seen_generation = current;
    used = pre_pos = pre_count = captured = pending_silence = vad_used = 0;
    vad_dc_q8 = 0; capture_finished = false;
    vad_reset_trigger(vad);
  }
  accept_requested_wake();
  for (size_t i = 0; i < count;) {
    if (seen_generation != atomic_load(&generation)) return;
    size_t n = frame - used;
    if (n > count - i) n = count - i;
    memcpy(mic + used, pcm + i, n * sizeof(*mic));
    int64_t now = esp_timer_get_time();
    unsigned missing = 0;
    portENTER_CRITICAL(&lock);
    for (size_t j = 0; j < n; ++j) {
      if (ref_read < ref_write) ref[used + j] = reference[ref_read++ % REF_CAP];
      else { ref[used + j] = 0; ++missing; }
    }
    bool echoing = atomic_load(&playing) || now < echo_tail_until;
    bool warmed = now >= warmup_until;
    portEXIT_CRITICAL(&lock);
    app_barge_diagnostic_add(seen_generation, BD_REF_MISSING, missing);
    used += n; i += n;
    if (used < frame) continue;
    used = 0;
    app_barge_diagnostic_add(seen_generation, BD_MIC, 1);
    app_barge_diagnostic_pcm(seen_generation, 0, mic, frame);
    app_barge_diagnostic_pcm(seen_generation, 1, ref, frame);
    if (echoing) aec_process(aec, mic, ref, clean);
    else memcpy(clean, mic, frame * sizeof(*clean));
    app_barge_diagnostic_add(seen_generation, BD_AEC, echoing ? 1 : 0);
    app_barge_diagnostic_pcm(seen_generation, 2, clean, frame);
    if (seen_generation != atomic_load(&generation)) return;
    for (size_t j = 0; j < frame; ++j) {
      if (!atomic_load(&pending)) {
        preroll[pre_pos] = clean[j];
        pre_pos = (pre_pos + 1) % PRE_CAP;
        if (pre_count < PRE_CAP) ++pre_count;
      } else if (!capture_finished && captured < CAPTURE_CAP) {
        pending_pcm[captured++] = clean[j];
        int32_t raw = clean[j];
        vad_dc_q8 += (raw * 256 - vad_dc_q8) / 256;
        int32_t scaled = (raw - vad_dc_q8 / 256) * 4;
        if (scaled > INT16_MAX) scaled = INT16_MAX;
        if (scaled < INT16_MIN) scaled = INT16_MIN;
        vad_frame[vad_used++] = (int16_t)scaled;
        if (vad_used == VAD_SAMPLES) {
          vad_used = 0;
          bool speech = vad_process_with_trigger(vad, vad_frame) == VAD_SPEECH;
          // Pre-roll and the old speaker tail are not evidence of a new question.
          // Keep their audio until real post-playback speech is confirmed.
          if (!echoing) {
            pending_voice_run = speech ? pending_voice_run + VAD_SAMPLES : 0;
            if (!pending_question && pending_voice_run >= RATE / 5) {
              size_t keep = captured < RATE * 320 / 1000 ? captured : RATE * 320 / 1000;
              memmove(pending_pcm, pending_pcm + captured - keep, keep * sizeof(*pending_pcm));
              captured = keep;
              pending_question = true;
            }
            pending_silence = speech ? 0 : pending_silence + VAD_SAMPLES;
          } else {
            pending_voice_run = pending_silence = 0;
          }
          capture_finished = pending_question && pending_silence >= SILENCE_SAMPLES;
        }
        if (captured == CAPTURE_CAP) capture_finished = true;
      }
    }
    if (atomic_load(&listening) && atomic_load(&playing) && !atomic_load(&pending) &&
        !atomic_load(&blocked) && warmed)
      app_wake_feed_barge(clean, frame, seen_generation);
    accept_requested_wake();
  }
}
bool app_acoustic_pending(void) {
  return atomic_load_explicit(&pending, memory_order_acquire);
}
size_t app_acoustic_take(int16_t *pcm, size_t capacity, size_t *silence_samples) {
  if (silence_samples) *silence_samples = 0;
  if (!pcm || !app_acoustic_pending()) return 0;
  size_t n = captured < capacity ? captured : capacity;
  if (!pending_question) n = 0;
  memcpy(pcm, pending_pcm, n * sizeof(*pcm));
  portENTER_CRITICAL(&lock);
  if (!atomic_load(&pending) || app_cloud_is_cancelled()) {
    n = 0;
  } else {
    if (silence_samples) *silence_samples = pending_silence;
    atomic_store(&pending, false);
    atomic_store(&listening, false);
    atomic_store(&requested, 0);
    atomic_fetch_add(&generation, 1);
  }
  portEXIT_CRITICAL(&lock);
  captured = pending_silence = pre_count = pre_pos = used = 0;
  capture_finished = false;
  return n;
}
#else
void app_acoustic_init(void) { app_barge_diagnostic_init(); }
bool app_acoustic_prepare(void) { return false; }
bool app_acoustic_listen(bool active) { return !active; }
void app_acoustic_discard(void) {}
void app_acoustic_playing(bool active) { (void)active; }
void app_acoustic_reference(const int16_t *pcm, size_t count) { (void)pcm; (void)count; }
void app_acoustic_feed(const int16_t *pcm, size_t count) { (void)pcm; (void)count; }
bool app_acoustic_wake_detected(unsigned epoch) { (void)epoch; return false; }
bool app_acoustic_pending(void) { return false; }
size_t app_acoustic_take(int16_t *pcm, size_t capacity, size_t *silence_samples) {
  (void)pcm; (void)capacity;
  if (silence_samples) *silence_samples = 0;
  return 0;
}
#endif
