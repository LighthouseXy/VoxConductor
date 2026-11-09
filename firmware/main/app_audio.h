#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#define APP_AUDIO_SAMPLE_RATE_HZ 16000
#define APP_AUDIO_RECORD_MAX_SECONDS 20
#define APP_AUDIO_FOLLOWUP_WAIT_SECONDS 12
#define APP_AUDIO_RECORD_MAX_SAMPLE_COUNT                                      \
  (APP_AUDIO_SAMPLE_RATE_HZ * APP_AUDIO_RECORD_MAX_SECONDS)
#define APP_AUDIO_RECORD_MAX_BYTE_COUNT                                        \
  (APP_AUDIO_RECORD_MAX_SAMPLE_COUNT * sizeof(int16_t))

typedef void (*app_audio_level_callback_t)(uint32_t level, void *user_data);

esp_err_t app_audio_init(void);
esp_err_t app_audio_start_level_monitor(app_audio_level_callback_t callback,
                                        void *user_data);
void app_audio_prepare_recording(void);
void app_audio_prepare_auto_recording(void);
void app_audio_request_record_stop(void);
esp_err_t app_audio_capture_recording(int16_t *recording,
                                      size_t *recorded_sample_count);
typedef void (*app_audio_capture_progress_t)(size_t samples, void *context);
// Follow-up waits locally for speech. Progress is unpublished until VAD hears
// speech; then it includes 320ms of pre-roll. Idle expiry returns NOT_FOUND.
esp_err_t app_audio_capture_progress(int16_t *recording, size_t *samples,
                                     app_audio_capture_progress_t progress,
                                     void *context, bool wait_for_speech);
esp_err_t app_audio_play_mono_pcm(const int16_t *mono_samples,
                                  size_t sample_count);
// Local media shares the PCM conversion/volume path but not cloud cancellation.
// Only the media worker may call this; priority callers suspend that worker
// first.
esp_err_t app_audio_play_media_pcm(const int16_t *mono_samples,
                                   size_t sample_count);
// 仅由播放任务调用；停止TX并清除DMA旧音频，不能从按键/LVGL回调调用。
esp_err_t app_audio_abort_playback(void);
// Playback owner only; reset before a turn, report after output stops.
void app_audio_playback_diagnostics(bool reset);
// Additional 16kHz reference padding relative to the original DMA depth.
unsigned app_audio_reference_extra_samples(void);
void app_audio_set_volume(unsigned percent);
unsigned app_audio_get_volume(void);
// 固定本地人声，不联网、不加提示蜂鸣；调用者持有音频独占权。
esp_err_t app_audio_local_reminder(void);
