#include "app_audio.h"

#include <stdatomic.h>
#include <stdbool.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#define MIC_SAMPLE_RATE_HZ APP_AUDIO_SAMPLE_RATE_HZ
#define MIC_PIN_SCK GPIO_NUM_8
#define MIC_PIN_WS GPIO_NUM_9
#define MIC_PIN_SD GPIO_NUM_14
#define MIC_READ_FRAMES 256
#define MIC_WORDS_PER_FRAME 2
#define SPEAKER_SAMPLE_RATE_HZ 24000
#define SPEAKER_PIN_BCLK GPIO_NUM_15
#define SPEAKER_PIN_LRC GPIO_NUM_16
#define SPEAKER_PIN_DIN GPIO_NUM_17
#define TTS_PLAYBACK_VOLUME_PERCENT 35
#define TTS_PLAYBACK_FRAMES_PER_BLOCK 256

#define RECORD_MAX_SECONDS APP_AUDIO_RECORD_MAX_SECONDS
#define RECORD_MAX_SAMPLE_COUNT APP_AUDIO_RECORD_MAX_SAMPLE_COUNT

static const char *TAG = "app_audio";
static i2s_chan_handle_t s_mic_rx_channel;
static i2s_chan_handle_t s_speaker_tx_channel;
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

  ESP_ERROR_CHECK(i2s_channel_enable(s_speaker_tx_channel));

  ESP_LOGI(TAG, "MAX98357A已启动：%d Hz，BCLK=%d，LRC=%d，DIN=%d",
           SPEAKER_SAMPLE_RATE_HZ, SPEAKER_PIN_BCLK, SPEAKER_PIN_LRC,
           SPEAKER_PIN_DIN);
}

esp_err_t app_audio_play_mono_pcm(const int16_t *mono_samples,
                               size_t sample_count) {
  int16_t stereo_samples[TTS_PLAYBACK_FRAMES_PER_BLOCK * 2];

  size_t offset = 0;

  while (offset < sample_count) {
    size_t frame_count = sample_count - offset;

    if (frame_count > TTS_PLAYBACK_FRAMES_PER_BLOCK) {
      frame_count = TTS_PLAYBACK_FRAMES_PER_BLOCK;
    }

    for (size_t i = 0; i < frame_count; i++) {
      // 软件音量控制，同时保留后续调节空间
      const int32_t scaled =
          ((int32_t)mono_samples[offset + i] * TTS_PLAYBACK_VOLUME_PERCENT) /
          100;

      // MAX98357A当前按立体声I2S接收，左右写入相同语音
      stereo_samples[i * 2] = (int16_t)scaled;
      stereo_samples[i * 2 + 1] = (int16_t)scaled;
    }

    const size_t write_bytes = frame_count * 2 * sizeof(int16_t);

    size_t bytes_written = 0;

    const esp_err_t result =
        i2s_channel_write(s_speaker_tx_channel, stereo_samples, write_bytes,
                          &bytes_written, 2000);

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

  return ESP_OK;
}


static void init_microphone(void) {
  // ESP32-S3作为I2S主机，为INMP441产生SCK和WS
  i2s_chan_config_t channel_config =
      I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);

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
  if (recording == NULL || recorded_sample_count == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  *recorded_sample_count = 0;

  ESP_LOGI(TAG, "录音已开始，保持按住说话，松开发送，最长%d秒",
           RECORD_MAX_SECONDS);

  if (xSemaphoreTake(s_mic_read_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
    ESP_LOGE(TAG, "等待麦克风I2S通道超时");
    return ESP_ERR_TIMEOUT;
  }

  esp_err_t result = ESP_OK;
  size_t bytes_read = 0;
  size_t recorded_samples = 0;
  bool stopped_by_button = false;

  // 丢弃音量检测任务残留在DMA中的旧数据
  for (int discard = 0; discard < 4; discard++) {
    result = i2s_channel_read(s_mic_rx_channel, s_mic_samples,
                              sizeof(s_mic_samples), &bytes_read, 1000);

    if (result != ESP_OK) {
      ESP_LOGE(TAG, "清理I2S缓冲区失败：%s", esp_err_to_name(result));
      break;
    }
  }

  while (result == ESP_OK && recorded_samples < RECORD_MAX_SAMPLE_COUNT) {
    bytes_read = 0;

    result = i2s_channel_read(s_mic_rx_channel, s_mic_samples,
                              sizeof(s_mic_samples), &bytes_read, 1000);

    if (result != ESP_OK) {
      ESP_LOGE(TAG, "录音读取失败：%s", esp_err_to_name(result));
      break;
    }

    const size_t word_count = bytes_read / sizeof(s_mic_samples[0]);

    const size_t frames_in_block = word_count / MIC_WORDS_PER_FRAME;

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

    // 至少录制1秒，避免产生过短、无法识别的音频
    if (recorded_samples >= MIC_SAMPLE_RATE_HZ &&
        atomic_load_explicit(&s_stop_requested, memory_order_acquire)) {
      stopped_by_button = true;
      break;
    }
  }

  xSemaphoreGive(s_mic_read_mutex);

  if (result != ESP_OK) {
    return result;
  }

  *recorded_sample_count = recorded_samples;

  const size_t recorded_bytes = recorded_samples * sizeof(int16_t);

  const unsigned recorded_ms =
      (unsigned)(recorded_samples * 1000 / MIC_SAMPLE_RATE_HZ);

  if (stopped_by_button) {
    ESP_LOGI(TAG, "检测到按键松开结束请求");
  } else {
    ESP_LOGW(TAG, "已达到%d秒录音上限", RECORD_MAX_SECONDS);
  }

  ESP_LOGI(TAG, "录音完成：%ums，%u字节", recorded_ms,
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

    // 读取完成后立即释放，让正式录音可以取得通道
    xSemaphoreGive(s_mic_read_mutex);

    if (result != ESP_OK) {
      ESP_LOGW(TAG, "麦克风读取失败：%s", esp_err_to_name(result));
      continue;
    }

    const size_t word_count = bytes_read / sizeof(s_mic_samples[0]);

    const size_t frames_in_block = word_count / MIC_WORDS_PER_FRAME;

    if (frames_in_block == 0) {
      continue;
    }

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
    emit_level(smoothed_level);
  }
}


esp_err_t app_audio_init(void) {
  init_speaker();
  init_microphone();

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

  const BaseType_t result = xTaskCreate(
      microphone_level_task, "mic_level", 4096, NULL, 5,
      &s_mic_level_task_handle);

  return result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_audio_prepare_recording(void) {
  atomic_store_explicit(&s_stop_requested, false, memory_order_release);
}

void app_audio_request_record_stop(void) {
  atomic_store_explicit(&s_stop_requested, true, memory_order_release);
}
