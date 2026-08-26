#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define APP_AUDIO_SAMPLE_RATE_HZ 16000
#define APP_AUDIO_RECORD_MAX_SECONDS 20
#define APP_AUDIO_RECORD_MAX_SAMPLE_COUNT \
  (APP_AUDIO_SAMPLE_RATE_HZ * APP_AUDIO_RECORD_MAX_SECONDS)
#define APP_AUDIO_RECORD_MAX_BYTE_COUNT \
  (APP_AUDIO_RECORD_MAX_SAMPLE_COUNT * sizeof(int16_t))

typedef void (*app_audio_level_callback_t)(uint32_t level, void *user_data);

esp_err_t app_audio_init(void);
esp_err_t app_audio_start_level_monitor(app_audio_level_callback_t callback,
                                        void *user_data);
void app_audio_prepare_recording(void);
void app_audio_request_record_stop(void);
esp_err_t app_audio_capture_recording(int16_t *recording,
                                      size_t *recorded_sample_count);
esp_err_t app_audio_play_mono_pcm(const int16_t *mono_samples,
                                  size_t sample_count);
