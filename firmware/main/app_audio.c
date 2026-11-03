/*
 * 音频硬件层：16 kHz采集、VAD句尾检测、24 kHz扬声器输出。
 * 发布已写好的连续采样；ASR消费原始PCM，检测使用独立增益/DC副本。
 * 互斥锁保护硬件所有权，取消播放须结束云端请求并清理DMA。
 */
#include "app_audio.h"
#include "app_acoustic.h"
#include "app_barge_diagnostic.h"
#include "app_cloud.h"
#include "app_log.h"
#include "app_wake.h"
#include "sdkconfig.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <stdatomic.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_vad.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MIC_SAMPLE_RATE_HZ APP_AUDIO_SAMPLE_RATE_HZ
#ifndef CONFIG_VOX_AUTO_END_SILENCE_MS
#define CONFIG_VOX_AUTO_END_SILENCE_MS 800
#endif
#ifndef CONFIG_VOX_AUTO_END_VAD_MODE
#define CONFIG_VOX_AUTO_END_VAD_MODE 0
#endif
#define AUTO_VAD_FRAME_MS 20
#define AUTO_VAD_FRAME_SAMPLES (MIC_SAMPLE_RATE_HZ * AUTO_VAD_FRAME_MS / 1000)
#define AUTO_VAD_GAIN 4
#define AUTO_START_CONFIRM_MS 200
#define AUTO_START_WAIT_SECONDS 8
#define FOLLOWUP_PREROLL_SAMPLES (MIC_SAMPLE_RATE_HZ * 320 / 1000)
#define MIC_PIN_SCK GPIO_NUM_2
#define MIC_PIN_WS GPIO_NUM_1
#define MIC_PIN_SD GPIO_NUM_42
#define MIC_READ_FRAMES 256
#define MIC_WORDS_PER_FRAME 2
#define MIC_DMA_DESC_COUNT 6
#define MIC_DMA_FRAME_COUNT 240
#define SPEAKER_SAMPLE_RATE_HZ 24000
#define SPEAKER_DMA_DESC_COUNT 8
#define SPEAKER_DMA_FRAME_COUNT 256
#define SPEAKER_PIN_BCLK GPIO_NUM_5
#define SPEAKER_PIN_LRC GPIO_NUM_6
#define SPEAKER_PIN_DIN GPIO_NUM_4
#define TTS_PLAYBACK_VOLUME_PERCENT 35
static _Atomic unsigned s_volume = TTS_PLAYBACK_VOLUME_PERCENT;
static _Atomic bool s_auto_record;
void app_audio_set_volume(unsigned percent) {
  atomic_store(&s_volume, percent > 100 ? 100 : percent);
}
unsigned app_audio_get_volume(void) { return atomic_load(&s_volume); }
#define TTS_PLAYBACK_FRAMES_PER_BLOCK SPEAKER_DMA_FRAME_COUNT

#define RECORD_MAX_SECONDS APP_AUDIO_RECORD_MAX_SECONDS
#define RECORD_MAX_SAMPLE_COUNT APP_AUDIO_RECORD_MAX_SAMPLE_COUNT

static const char *TAG = "app_audio";
static i2s_chan_handle_t s_mic_rx_channel;
static i2s_chan_handle_t s_speaker_tx_channel;
static bool s_speaker_running;
static bool s_speaker_dma_clean;
static size_t s_silence_loaded;
static int64_t s_convert_max_us, s_reference_max_us, s_i2s_max_us;
static DRAM_ATTR portMUX_TYPE s_output_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR struct {
  bool active;
  int64_t last_eof;
  unsigned eof_max_us, queue_overflows;
} s_output_dma;
static bool s_output_measure;

static bool IRAM_ATTR speaker_sent(i2s_chan_handle_t channel,
                                   i2s_event_data_t *event, void *context) {
  (void)channel; (void)event; (void)context;
  int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&s_output_lock);
  if (s_output_dma.active) {
    if (s_output_dma.last_eof) {
      unsigned gap = (unsigned)(now - s_output_dma.last_eof);
      if (gap > s_output_dma.eof_max_us) s_output_dma.eof_max_us = gap;
    }
    s_output_dma.last_eof = now;
  }
  portEXIT_CRITICAL_ISR(&s_output_lock);
  return false;
}
static bool IRAM_ATTR speaker_overflow(i2s_chan_handle_t channel,
                                       i2s_event_data_t *event, void *context) {
  (void)channel; (void)event; (void)context;
  portENTER_CRITICAL_ISR(&s_output_lock);
  if (s_output_dma.active) ++s_output_dma.queue_overflows;
  portEXIT_CRITICAL_ISR(&s_output_lock);
  return false;
}
unsigned app_audio_reference_extra_samples(void) {
  // Preserve the existing reference offset relative to the old 4x160-frame DMA.
  return (SPEAKER_DMA_DESC_COUNT * SPEAKER_DMA_FRAME_COUNT - 4 * 160) *
         APP_AUDIO_SAMPLE_RATE_HZ / SPEAKER_SAMPLE_RATE_HZ;
}
void app_audio_playback_diagnostics(bool reset) {
  portENTER_CRITICAL(&s_output_lock);
  unsigned eof_max = s_output_dma.eof_max_us;
  unsigned overflows = s_output_dma.queue_overflows;
  s_output_dma.active = false;
  s_output_dma.last_eof = 0;
  s_output_dma.eof_max_us = s_output_dma.queue_overflows = 0;
  portEXIT_CRITICAL(&s_output_lock);
  s_output_measure = reset;
  if (reset) {
    s_convert_max_us = s_reference_max_us = s_i2s_max_us = 0;
    return;
  }
  ESP_LOGI(TAG, "output_diag: convert_max=%uus reference_max=%uus i2s_max=%uus dma_gap_max=%uus dma_q_overflow=%u",
           (unsigned)s_convert_max_us, (unsigned)s_reference_max_us,
           (unsigned)s_i2s_max_us, eof_max, overflows);
}
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
static int16_t s_resample_group[3];
static unsigned s_resample_phase;
#endif
static SemaphoreHandle_t s_mic_read_mutex;
static TaskHandle_t s_mic_level_task_handle;
static int32_t s_mic_samples[MIC_READ_FRAMES * MIC_WORDS_PER_FRAME];
static _Atomic uint32_t s_mic_level;
static _Atomic bool s_stop_requested;
static app_audio_level_callback_t s_level_callback;
static void *s_level_user_data;

static void emit_level(uint32_t level) {
  if (s_level_callback != NULL) {
    s_level_callback(level, s_level_user_data);
  }
}

static void init_speaker(void) {
  // 为MAX98357A创建一个I2S发送通道
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

  // 没有新数据时自动输出零，避免停播后残留杂音
  channel_config.auto_clear_after_cb = true;
  channel_config.dma_desc_num = SPEAKER_DMA_DESC_COUNT;
  channel_config.dma_frame_num = SPEAKER_DMA_FRAME_COUNT;
  channel_config.intr_priority = 3;

  ESP_ERROR_CHECK(
      i2s_new_channel(&channel_config, &s_speaker_tx_channel, NULL));

  i2s_std_config_t std_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(SPEAKER_SAMPLE_RATE_HZ),

      // MAX98357A接收标准Philips I2S；左右声道发送相同数据
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                      I2S_SLOT_MODE_STEREO),

      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = SPEAKER_PIN_BCLK,
              .ws = SPEAKER_PIN_LRC,
              .dout = SPEAKER_PIN_DIN,
              .din = I2S_GPIO_UNUSED,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_speaker_tx_channel, &std_config));
  const i2s_event_callbacks_t callbacks = {
      .on_sent = speaker_sent, .on_send_q_ovf = speaker_overflow};
  ESP_ERROR_CHECK(i2s_channel_register_event_callback(s_speaker_tx_channel,
                                                     &callbacks, NULL));

  ESP_ERROR_CHECK(i2s_channel_enable(s_speaker_tx_channel));
  s_speaker_running = true;

  ESP_LOGI(TAG, "MAX98357A已启动：%d Hz，BCLK=%d，LRC=%d，DIN=%d",
           SPEAKER_SAMPLE_RATE_HZ, SPEAKER_PIN_BCLK, SPEAKER_PIN_LRC,
           SPEAKER_PIN_DIN);
}

esp_err_t app_audio_abort_playback(void) {
  portENTER_CRITICAL(&s_output_lock);
  s_output_dma.active = false;
  portEXIT_CRITICAL(&s_output_lock);
  if (!s_speaker_tx_channel)
    return ESP_OK;
  if (!s_speaker_running && s_speaker_dma_clean)
    return ESP_OK;
  esp_err_t result = ESP_OK;
  if (s_speaker_running) {
    result = i2s_channel_disable(s_speaker_tx_channel);
    if (result != ESP_OK)
      return result;
    s_speaker_running = false;
    s_silence_loaded = 0;
    s_speaker_dma_clean = false;
  }
  // A failed preload may be retried while TX is already disabled. Acknowledge
  // the audio handoff after both disabling and preloading succeed.
  // Size follows the DMA configuration, including stereo expansion.
  static const int16_t silence[SPEAKER_DMA_DESC_COUNT * SPEAKER_DMA_FRAME_COUNT * 2] = {0};
  size_t loaded = 0;
  result =
      i2s_channel_preload_data(s_speaker_tx_channel, silence,
                               sizeof(silence) - s_silence_loaded, &loaded);
  if (loaded > sizeof(silence) - s_silence_loaded)
    return ESP_FAIL;
  s_silence_loaded += loaded;
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
  s_resample_phase = 0;
#endif
  s_speaker_dma_clean = result == ESP_OK && s_silence_loaded == sizeof(silence);
  if (result == ESP_OK && !s_speaker_dma_clean)
    result = ESP_FAIL;
  if (result == ESP_OK)
    APP_LOG_DETAIL(TAG, "终止播放并清空DMA：%s，清除=%u B", esp_err_to_name(result),
                   (unsigned)loaded);
  else
    ESP_LOGW(TAG, "清空扬声器DMA失败：%s", esp_err_to_name(result));
  return result;
}

static esp_err_t play_pcm(const int16_t *mono_samples, size_t sample_count,
                          bool cancellable) {
  if (!s_speaker_tx_channel || (!mono_samples && sample_count))
    return ESP_ERR_INVALID_ARG;
  if (cancellable && app_cloud_is_cancelled()) return ESP_ERR_INVALID_STATE;
  if (!s_speaker_running) {
    esp_err_t result = i2s_channel_enable(s_speaker_tx_channel);
    if (result != ESP_OK)
      return result;
    s_speaker_running = true;
  }
  s_speaker_dma_clean = false;
  s_silence_loaded = 0;
  int16_t stereo_samples[TTS_PLAYBACK_FRAMES_PER_BLOCK * 2];
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
  int16_t reference[TTS_PLAYBACK_FRAMES_PER_BLOCK];
#endif

  size_t offset = 0;

  while (offset < sample_count) {
    if (cancellable && app_cloud_is_cancelled()) return ESP_ERR_INVALID_STATE;
    size_t frame_count = sample_count - offset;

    if (frame_count > TTS_PLAYBACK_FRAMES_PER_BLOCK) {
      frame_count = TTS_PLAYBACK_FRAMES_PER_BLOCK;
    }

    size_t output_frames = 0;
    int64_t convert_started = esp_timer_get_time();
    const unsigned volume = atomic_load(&s_volume);
    for (size_t i = 0; i < frame_count; i++) {
      // 软件音量控制，同时保留后续调节空间
      const int32_t scaled =
          ((int32_t)mono_samples[offset + i] * (int32_t)volume) / 100;

      // MAX98357A当前按立体声I2S接收，左右写入相同语音
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
      // Downsample only the post-volume reference. Actual speaker output stays
      // 24kHz, including music/reminders; preserve phase across small blocks.
      s_resample_group[s_resample_phase++] = (int16_t)scaled;
      if (s_resample_phase == 3) {
        s_resample_phase = 0;
        reference[output_frames] = s_resample_group[0];
        ++output_frames;
        int16_t middle =
            (int16_t)(((int32_t)s_resample_group[1] + s_resample_group[2]) / 2);
        reference[output_frames] = middle;
        ++output_frames;
      }
#endif
      stereo_samples[i * 2] = (int16_t)scaled;
      stereo_samples[i * 2 + 1] = (int16_t)scaled;
    }

    const size_t write_bytes = frame_count * 2 * sizeof(int16_t);
    int64_t reference_started = esp_timer_get_time();
    if (reference_started - convert_started > s_convert_max_us)
      s_convert_max_us = reference_started - convert_started;
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
    app_acoustic_reference(reference, output_frames);
#else
    (void)output_frames;
#endif

    size_t bytes_written = 0;
    if (s_output_measure) {
      portENTER_CRITICAL(&s_output_lock);
      s_output_dma.active = true;
      portEXIT_CRITICAL(&s_output_lock);
    }
    int64_t i2s_started = esp_timer_get_time();
    if (i2s_started - reference_started > s_reference_max_us)
      s_reference_max_us = i2s_started - reference_started;

    const esp_err_t result =
        i2s_channel_write(s_speaker_tx_channel, stereo_samples, write_bytes,
                          &bytes_written, 2000);
    int64_t i2s_elapsed = esp_timer_get_time() - i2s_started;
    if (i2s_elapsed > s_i2s_max_us) s_i2s_max_us = i2s_elapsed;

    if (result != ESP_OK) {
      ESP_LOGE(TAG, "TTS音频写入I2S失败：%s", esp_err_to_name(result));
      return result;
    }

    if (bytes_written != write_bytes) {
      ESP_LOGE(TAG, "TTS音频写入不完整");
      return ESP_FAIL;
    }

    offset += frame_count;
  }

  return cancellable && app_cloud_is_cancelled() ? ESP_ERR_INVALID_STATE : ESP_OK;
}

esp_err_t app_audio_play_mono_pcm(const int16_t *samples, size_t count) {
  return play_pcm(samples, count, true);
}
esp_err_t app_audio_play_media_pcm(const int16_t *samples, size_t count) {
  return play_pcm(samples, count, false);
}

esp_err_t app_audio_local_reminder(void) {
  extern const uint8_t start[] asm("_binary_reminder_pcm_start");
  extern const uint8_t end[] asm("_binary_reminder_pcm_end");
  UBaseType_t priority = uxTaskPriorityGet(NULL);
  vTaskPrioritySet(NULL, 5);
  esp_err_t result =
      app_audio_play_mono_pcm((const int16_t *)start, (end - start) / 2);
  // 提交静音尾部排出DMA中的最后一句，不在语音前加蜂鸣。
  const int16_t tail[480] = {0};
  for (unsigned i = 0; i < 8 && result == ESP_OK; i++)
    result = app_audio_play_mono_pcm(tail, 480);
  if (result != ESP_OK)
    (void)app_audio_abort_playback();
  vTaskPrioritySet(NULL, priority);
  return result;
}

static void init_microphone(void) {
  // ESP32-S3作为I2S主机，为INMP441产生SCK和WS
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

  channel_config.dma_desc_num = MIC_DMA_DESC_COUNT;
  channel_config.dma_frame_num = MIC_DMA_FRAME_COUNT;
  ESP_ERROR_CHECK(i2s_new_channel(&channel_config, NULL, &s_mic_rx_channel));

  i2s_std_config_t std_config = {
      .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(MIC_SAMPLE_RATE_HZ),

      // INMP441输出24位数据，使用32位槽接收最容易保证对齐
      .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,
                                                      I2S_SLOT_MODE_STEREO),

      .gpio_cfg =
          {
              .mclk = I2S_GPIO_UNUSED,
              .bclk = MIC_PIN_SCK,
              .ws = MIC_PIN_WS,
              .dout = I2S_GPIO_UNUSED,
              .din = MIC_PIN_SD,
              .invert_flags =
                  {
                      .mclk_inv = false,
                      .bclk_inv = false,
                      .ws_inv = false,
                  },
          },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(s_mic_rx_channel, &std_config));

  ESP_ERROR_CHECK(i2s_channel_enable(s_mic_rx_channel));

  ESP_LOGI(TAG, "INMP441已启动：%d Hz，SCK=%d，WS=%d，SD=%d",
           MIC_SAMPLE_RATE_HZ, MIC_PIN_SCK, MIC_PIN_WS, MIC_PIN_SD);
}

esp_err_t app_audio_capture_recording(int16_t *recording,
                                      size_t *recorded_sample_count) {
  return app_audio_capture_progress(recording, recorded_sample_count, NULL,
                                    NULL, false);
}

esp_err_t app_audio_capture_progress(int16_t *recording,
                                     size_t *recorded_sample_count,
                                     app_audio_capture_progress_t progress,
                                     void *context, bool wait_for_speech) {
  if (recording == NULL || recorded_sample_count == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *recorded_sample_count = 0;

  if (xSemaphoreTake(s_mic_read_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "等待麦克风I2S通道超时");
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t result = ESP_OK;
  size_t bytes_read = 0;
  size_t silence_samples = 0;
  bool interrupted_capture = app_acoustic_pending();
  // An accepted wake only stops the answer; wait for a separate speech onset.
  // take() returns retained audio only when post-playback speech was confirmed.
  if (interrupted_capture) wait_for_speech = true;
  size_t recorded_samples =
      app_acoustic_take(recording, RECORD_MAX_SAMPLE_COUNT, &silence_samples);
  bool automatic =
      atomic_exchange(&s_auto_record, false) || recorded_samples > 0 ||
      wait_for_speech;
  // The bundled VAD allocator assumes malloc succeeds (about 1 KiB total).
  // Avoid calling it under severe heap pressure; manual capture still works.
  if (automatic && heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT) < 4096) {
    xSemaphoreGive(s_mic_read_mutex);
    ESP_LOGE(TAG, "Automatic recording: insufficient heap for VAD");
    return ESP_ERR_NO_MEM;
  }
  // Use the installed ESP-SR voice detector for local speech decisions.
  // Only this private VAD copy is DC-filtered/amplified; ASR PCM is unchanged.
  vad_handle_t vad = automatic ? vad_create_with_param(
      (vad_mode_t)CONFIG_VOX_AUTO_END_VAD_MODE, MIC_SAMPLE_RATE_HZ,
      AUTO_VAD_FRAME_MS, 80, AUTO_VAD_FRAME_MS) : NULL;
  if (automatic && !vad) {
    xSemaphoreGive(s_mic_read_mutex);
    ESP_LOGE(TAG, "Automatic recording: VAD allocation failed");
    return ESP_ERR_NO_MEM;
  }
  int16_t vad_frame[AUTO_VAD_FRAME_SAMPLES];
  size_t vad_used = 0;
  int32_t vad_dc_q8 = 0;
  unsigned pcm_mean_max = 0;
  unsigned speech_run_ms = 0, speech_run_max_ms = 0;
  bool stopped_without_activity = false;
  bool had_speech = recorded_samples > 0;
  bool waiting = wait_for_speech && !had_speech;
  size_t tail_guard = interrupted_capture && !had_speech ? MIC_SAMPLE_RATE_HZ / 5 : 0;
  size_t waiting_samples = 0;
  if (automatic)
    APP_LOG_DETAIL(TAG, "Automatic recording started: VAD mode=%d, gain=%dx, silence=%dms, max=%ds",
             CONFIG_VOX_AUTO_END_VAD_MODE, AUTO_VAD_GAIN,
             CONFIG_VOX_AUTO_END_SILENCE_MS, RECORD_MAX_SECONDS);
  else
    APP_LOG_DETAIL(TAG, "录音已开始，保持按住说话，松开发送，最长%d秒",
             RECORD_MAX_SECONDS);
  if (progress && recorded_samples)
    progress(recorded_samples, context);
  bool stopped_by_button = false;
  // Old cloud cleanup may outlast the whole utterance. Submit a completed
  // pending recording immediately instead of adding another silence wait.
  bool stopped_by_silence = had_speech && silence_samples >=
      MIC_SAMPLE_RATE_HZ * CONFIG_VOX_AUTO_END_SILENCE_MS / 1000;

  // 丢弃音量检测任务残留在DMA中的旧数据
  for (int discard = 0; !automatic && discard < 4; discard++) {
    result = i2s_channel_read(s_mic_rx_channel, s_mic_samples,
                              sizeof(s_mic_samples), &bytes_read, 1000);

    if (result != ESP_OK) {
      ESP_LOGE(TAG, "清理I2S缓冲区失败：%s", esp_err_to_name(result));
      break;
    }
  }

  while (result == ESP_OK && !stopped_by_silence &&
         recorded_samples < RECORD_MAX_SAMPLE_COUNT) {
    bytes_read = 0;

    result = i2s_channel_read(s_mic_rx_channel, s_mic_samples,
                              sizeof(s_mic_samples), &bytes_read, 1000);

    if (result != ESP_OK) {
      ESP_LOGE(TAG, "录音读取失败：%s", esp_err_to_name(result));
      break;
    }

    const size_t word_count = bytes_read / sizeof(s_mic_samples[0]);

    const size_t frames_in_block = word_count / MIC_WORDS_PER_FRAME;

    if (tail_guard) {
      if (atomic_load(&s_stop_requested) || app_cloud_is_cancelled()) {
        result = ESP_ERR_INVALID_STATE;
        break;
      }
      tail_guard = frames_in_block >= tail_guard ? 0 : tail_guard - frames_in_block;
      continue;
    }
    if (frames_in_block > 0) {
      int64_t block_sum = 0;

      // 先计算平均值，去除数字麦克风的直流偏移
      for (size_t i = 0; i + 1 < word_count; i += MIC_WORDS_PER_FRAME) {
        block_sum += s_mic_samples[i] >> 8;
      }

      const int32_t block_mean =
          (int32_t)(block_sum / (int64_t)frames_in_block);

      uint64_t block_ac_sum = 0;

      for (size_t i = 0; i + 1 < word_count; i += MIC_WORDS_PER_FRAME) {
        const int32_t sample = s_mic_samples[i] >> 8;

        const int32_t centered = sample - block_mean;

        const int32_t magnitude = centered >= 0 ? centered : -centered;

        block_ac_sum += (uint32_t)magnitude;
      }

      const uint32_t block_level = (uint32_t)(block_ac_sum / frames_in_block);

      const uint32_t previous_level =
          atomic_load_explicit(&s_mic_level, memory_order_relaxed);

      const uint32_t smoothed_level =
          previous_level == 0 ? block_level
                              : (previous_level * 3 + block_level) / 4;

      atomic_store_explicit(&s_mic_level, smoothed_level, memory_order_relaxed);
      emit_level(smoothed_level);
    }

    // INMP441当前使用左声道，每帧取第一个采样并转换为16位PCM
    for (size_t i = 0;
         i + 1 < word_count && recorded_samples < RECORD_MAX_SAMPLE_COUNT;
         i += MIC_WORDS_PER_FRAME) {
      recording[recorded_samples++] = (int16_t)(s_mic_samples[i] >> 16);
    }

    if (app_cloud_is_cancelled()) {
      result = ESP_ERR_INVALID_STATE;
      break;
    }
    if (automatic) {
      uint64_t amplitude = 0;
      for (size_t i = 0; i + 1 < word_count; i += MIC_WORDS_PER_FRAME) {
        int32_t raw = s_mic_samples[i] >> 16;
        amplitude += (unsigned)abs((int)raw);
        vad_dc_q8 += (raw * 256 - vad_dc_q8) / 256;
        int32_t scaled = (raw - vad_dc_q8 / 256) * AUTO_VAD_GAIN;
        if (scaled > INT16_MAX) scaled = INT16_MAX;
        if (scaled < INT16_MIN) scaled = INT16_MIN;
        vad_frame[vad_used++] = (int16_t)scaled;
        // DMA blocks contain 256 samples; VAD needs exactly 320 (20ms).
        if (vad_used != AUTO_VAD_FRAME_SAMPLES) continue;
        vad_used = 0;
        if (vad_process_with_trigger(vad, vad_frame) == VAD_SPEECH) {
          speech_run_ms += AUTO_VAD_FRAME_MS;
          if (speech_run_ms > speech_run_max_ms) speech_run_max_ms = speech_run_ms;
          // 短噪声/唤醒词尾音不能单独开启句尾静音计时。
          // Retained barge-in audio has already passed post-playback VAD.
          if (speech_run_ms >= AUTO_START_CONFIRM_MS) had_speech = true;
          silence_samples = 0;
        } else {
          speech_run_ms = 0;
          silence_samples += AUTO_VAD_FRAME_SAMPLES;
        }
      }
      unsigned mean = frames_in_block ? (unsigned)(amplitude / frames_in_block) : 0;
      if (mean > pcm_mean_max) pcm_mean_max = mean;
      if (waiting) {
        waiting_samples += frames_in_block;
        if (had_speech) {
          // Nothing was published while waiting, so compaction is safe.
          // Retain audio from the start of the utterance.
          size_t keep = recorded_samples < FOLLOWUP_PREROLL_SAMPLES
                            ? recorded_samples : FOLLOWUP_PREROLL_SAMPLES;
          memmove(recording, recording + recorded_samples - keep,
                  keep * sizeof(*recording));
          recorded_samples = keep;
          waiting = false;
          APP_LOG_DETAIL(TAG, "Follow-up speech detected; pre-roll=%u samples",
                   (unsigned)keep);
        }
      }
    }
    if (progress && !waiting)
      progress(recorded_samples, context);
    if (automatic) {
      if (had_speech && silence_samples >= MIC_SAMPLE_RATE_HZ *
                                               CONFIG_VOX_AUTO_END_SILENCE_MS /
                                               1000) {
        stopped_by_silence = true;
        break;
      }
      if (waiting && !speech_run_ms && waiting_samples >=
                         MIC_SAMPLE_RATE_HZ * APP_AUDIO_FOLLOWUP_WAIT_SECONDS) {
        result = ESP_ERR_NOT_FOUND;
        break;
      }
      if (!wait_for_speech && !had_speech && recorded_samples >=
                             MIC_SAMPLE_RATE_HZ * AUTO_START_WAIT_SECONDS) {
        // Cloud ASR may recover audio despite a local VAD false negative.
        // Submit the captured window; only a truly empty ASR result fails.
        stopped_without_activity = true;
        break;
      }
    }
    // 手动松开至少录制1秒；自动录音的清理请求必须及时响应。
    if ((automatic || recorded_samples >= MIC_SAMPLE_RATE_HZ) &&
        atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
      stopped_by_button = true;
      if (waiting) result = ESP_ERR_NOT_FOUND;
      break;
    }
  }

  if (vad) vad_destroy(vad);
  xSemaphoreGive(s_mic_read_mutex);

  *recorded_sample_count = recorded_samples;
  if (wait_for_speech && result == ESP_ERR_NOT_FOUND) {
    APP_LOG_DETAIL(TAG, "Follow-up window closed: quiet expiry; capture kept local");
    return result;
  }
  if (result != ESP_OK) {
    if (app_cloud_is_cancelled())
      APP_LOG_DETAIL(TAG, "Recording cancelled: mode=%s, samples=%u, status=%s",
                     automatic ? "automatic" : "button", (unsigned)recorded_samples,
                     esp_err_to_name(result));
    else
      ESP_LOGW(TAG, "Recording failed: mode=%s, samples=%u, status=%s",
               automatic ? "automatic" : "button", (unsigned)recorded_samples,
               esp_err_to_name(result));
    return result;
  }

  const size_t recorded_bytes = recorded_samples * sizeof(int16_t);

  const unsigned recorded_ms =
      (unsigned)(recorded_samples * 1000 / MIC_SAMPLE_RATE_HZ);

  if (automatic)
    APP_LOG_DETAIL(TAG, "Automatic capture: end=%s, speech=%d, duration=%ums, pcm_mean_max=%u, speech_run_max=%ums",
             stopped_without_activity ? "no_activity_submit" :
             stopped_by_silence ? "vad_silence" :
             stopped_by_button ? "stop_request" : "duration_limit",
             (int)had_speech, recorded_ms, pcm_mean_max, speech_run_max_ms);
  if (stopped_without_activity) {
    ESP_LOGW(TAG, "Local VAD found no speech; submitting recorded audio to ASR");
  } else if (stopped_by_button) {
    APP_LOG_DETAIL(TAG, "检测到按键松开结束请求");
  } else if (stopped_by_silence) {
    APP_LOG_DETAIL(TAG, "自动录音静音结束：尾长%dms", CONFIG_VOX_AUTO_END_SILENCE_MS);
  } else {
    ESP_LOGW(TAG, "已达到%d秒录音上限", RECORD_MAX_SECONDS);
  }

  APP_LOG_DETAIL(TAG, "录音完成：%ums，%u字节", recorded_ms,
           (unsigned)recorded_bytes);

  return ESP_OK;
}

static void microphone_level_task(void *argument) {
  (void)argument;

  while (true) {
    size_t bytes_read = 0;

    if (xSemaphoreTake(s_mic_read_mutex, portMAX_DELAY) != pdTRUE) {
      continue;
    }

    esp_err_t result =
        i2s_channel_read(s_mic_rx_channel, s_mic_samples, sizeof(s_mic_samples),
                         &bytes_read, 1000);

    if (result != ESP_OK) {
      xSemaphoreGive(s_mic_read_mutex);
      ESP_LOGW(TAG, "麦克风读取失败：%s", esp_err_to_name(result));
      continue;
    }

    const size_t word_count = bytes_read / sizeof(s_mic_samples[0]);

    const size_t frames_in_block = word_count / MIC_WORDS_PER_FRAME;

    if (frames_in_block == 0) {
      xSemaphoreGive(s_mic_read_mutex);
      continue;
    }
    int16_t pcm[MIC_READ_FRAMES];
    for (size_t i = 0; i < frames_in_block; ++i)
      pcm[i] = (int16_t)(s_mic_samples[i * 2] >> 16);
    app_barge_diagnostic_input();
    app_acoustic_feed(pcm, frames_in_block);
    app_wake_feed(pcm, frames_in_block);

    // 第一遍计算本数据块的平均值，用于去除直流偏移
    int64_t block_sum = 0;

    for (size_t i = 0; i + 1 < word_count; i += 2) {
      block_sum += s_mic_samples[i] >> 8; // L/R接GND，读取左声道
    }

    const int32_t block_mean = (int32_t)(block_sum / (int64_t)frames_in_block);

    uint64_t block_ac_sum = 0;
    // 第二遍统计真正随声音变化的交流幅度
    for (size_t i = 0; i + 1 < word_count; i += 2) {
      const int32_t sample = s_mic_samples[i] >> 8;
      const int32_t centered_sample = sample - block_mean;
      const int32_t magnitude =
          centered_sample >= 0 ? centered_sample : -centered_sample;

      block_ac_sum += (uint32_t)magnitude;
    }

    const uint32_t block_level = (uint32_t)(block_ac_sum / frames_in_block);

    const uint32_t previous_level =
        atomic_load_explicit(&s_mic_level, memory_order_relaxed);

    // 简单低通平滑，避免音量条跳动过于剧烈
    const uint32_t smoothed_level =
        previous_level == 0 ? block_level
                            : (previous_level * 3 + block_level) / 4;

    atomic_store_explicit(&s_mic_level, smoothed_level, memory_order_relaxed);
    xSemaphoreGive(s_mic_read_mutex);
    emit_level(smoothed_level);
  }
}

esp_err_t app_audio_init(void) {
  init_speaker();
  init_microphone();
  app_acoustic_init();

  s_mic_read_mutex = xSemaphoreCreateMutex();
  if (s_mic_read_mutex == NULL) {
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t app_audio_start_level_monitor(app_audio_level_callback_t callback,
                                        void *user_data) {
  if (callback == NULL || s_mic_read_mutex == NULL) {
    return ESP_ERR_INVALID_STATE;
  }

  s_level_callback = callback;
  s_level_user_data = user_data;

  const BaseType_t result =
      xTaskCreate(microphone_level_task, "mic_level", 8192, NULL, 5,
                  &s_mic_level_task_handle);

  return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_audio_prepare_recording(void) {
  atomic_store(&s_auto_record, false);
  atomic_store_explicit(&s_stop_requested, false, memory_order_release);
}
void app_audio_prepare_auto_recording(void) {
  app_audio_prepare_recording();
  atomic_store(&s_auto_record, true);
}

void app_audio_request_record_stop(void) {
  atomic_store_explicit(&s_stop_requested, true, memory_order_release);
}
