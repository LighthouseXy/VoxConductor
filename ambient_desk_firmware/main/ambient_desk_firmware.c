#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "app_button.h"
#include "app_display.h"
#include "app_network.h"
#include "app_storage.h"
#include "app_ui.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#define TURN_ID_BUFFER_SIZE 48

static const char *TAG = "ambient_desk";

static _Atomic bool s_voice_turn_active = false;
static _Atomic bool s_voice_ready = false;
static uint32_t s_turn_sequence;

static void voice_conversation_task(void *argument);

static void update_mic_level(uint32_t level, void *user_data) {
  (void)user_data;
  app_ui_set_mic_level(level);
}

static void create_turn_id(char *buffer, size_t buffer_size) {
  const time_t now = time(NULL);
  const uint32_t sequence = ++s_turn_sequence;
  const uint32_t tick = (uint32_t)xTaskGetTickCount();

  // 时间、轮次和启动后时钟共同降低设备重启后的ID重复概率。
  snprintf(buffer, buffer_size, "%lld-%" PRIu32 "-%" PRIu32,
           (long long)now, sequence, tick);
}

static bool handle_button_event(app_button_event_t event, void *user_data) {
  (void)user_data;

  if (event == APP_BUTTON_EVENT_SHORT_PRESS) {
    if (atomic_load_explicit(&s_voice_turn_active, memory_order_acquire)) {
      ESP_LOGW(TAG, "语音对话正在处理，暂不切换页面");
      return false;
    }
    app_ui_show_next_page();
    return true;
  }

  if (event == APP_BUTTON_EVENT_LONG_PRESS_END) {
    // 松开事件可能早于录音任务真正开始，结束请求不能丢失。
    app_audio_request_record_stop();
    ESP_LOGI(TAG, "长按已松开，请求结束录音");
    return true;
  }

  if (!atomic_load_explicit(&s_voice_ready, memory_order_acquire)) {
    ESP_LOGW(TAG, "语音系统仍在初始化，请稍后再试");
    return false;
  }

  if (atomic_exchange_explicit(&s_voice_turn_active, true,
                               memory_order_acq_rel)) {
    ESP_LOGW(TAG, "语音对话正在处理，本次长按已忽略");
    return false;
  }

  app_audio_prepare_recording();
  const BaseType_t task_result =
      xTaskCreate(voice_conversation_task, "voice_turn", 8192, NULL, 6, NULL);
  if (task_result != pdPASS) {
    atomic_store_explicit(&s_voice_turn_active, false, memory_order_release);
    ESP_LOGE(TAG, "创建语音对话任务失败");
    return false;
  }

  ESP_LOGI(TAG, "检测到长按，开始语音录制");
  return true;
}

static esp_err_t record_and_upload_audio(void) {
  char turn_id[TURN_ID_BUFFER_SIZE];
  create_turn_id(turn_id, sizeof(turn_id));
  ESP_LOGI(TAG, "开始语音轮次：turn=%s", turn_id);

  const size_t free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  if (free_psram < APP_AUDIO_RECORD_MAX_BYTE_COUNT) {
    ESP_LOGE(TAG, "PSRAM不足，无法创建录音缓冲区");
    return ESP_ERR_NO_MEM;
  }

  int16_t *recording = heap_caps_malloc(APP_AUDIO_RECORD_MAX_BYTE_COUNT,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  if (recording == NULL) {
    return ESP_ERR_NO_MEM;
  }

  // 录音真正开始前显示聆听界面，圆环会随麦克风音量变化。
  app_ui_show_voice_state(APP_UI_VOICE_LISTENING, "松开发送");

  size_t recorded_samples = 0;
  esp_err_t result =
      app_audio_capture_recording(recording, &recorded_samples);

  if (result == ESP_OK) {
    app_ui_show_voice_state(APP_UI_VOICE_THINKING, "正在上传录音");
    result = app_network_upload_voice_turn(
        recording, recorded_samples * sizeof(int16_t), turn_id);
  }
  heap_caps_free(recording);

  if (result == ESP_OK) {
    const esp_err_t log_result =
        app_network_fetch_and_store_turn(turn_id);
    if (log_result != ESP_OK) {
      // 记录失败不影响已经完成的语音回答。
      ESP_LOGW(TAG, "本轮语音成功，但SD卡记录失败：%s",
               esp_err_to_name(log_result));
    }
  }

  if (result != ESP_OK) {
    app_ui_show_voice_state(APP_UI_VOICE_ERROR, "语音请求失败");
    vTaskDelay(pdMS_TO_TICKS(3000));
  }
  app_ui_hide_voice_state();
  return result;
}

static void voice_conversation_task(void *argument) {
  (void)argument;

  if (!app_network_is_connected()) {
    ESP_LOGW(TAG, "Wi-Fi尚未连接，无法开始语音对话");
    app_ui_show_voice_state(APP_UI_VOICE_ERROR, "网络未连接");
    vTaskDelay(pdMS_TO_TICKS(3000));
    app_ui_hide_voice_state();
  } else {
    const esp_err_t result = record_and_upload_audio();
    if (result == ESP_OK) {
      ESP_LOGI(TAG, "本次语音对话完成");
    } else {
      ESP_LOGW(TAG, "本次语音对话失败：%s", esp_err_to_name(result));
    }
  }

  atomic_store_explicit(&s_voice_turn_active, false, memory_order_release);
  vTaskDelete(NULL);
}

void app_main(void) {
  ESP_ERROR_CHECK(app_display_init());
  ESP_ERROR_CHECK(app_ui_init());
  ESP_ERROR_CHECK(app_button_init(handle_button_event, NULL));

  const esp_err_t storage_result = app_storage_init();
  const bool storage_ready = storage_result == ESP_OK;
  if (!storage_ready) {
    ESP_LOGW(TAG, "microSD挂载失败，继续启动其他功能：%s",
             esp_err_to_name(storage_result));
  }

  ESP_ERROR_CHECK(app_audio_init());

  const esp_err_t monitor_result =
      app_audio_start_level_monitor(update_mic_level, NULL);
  if (monitor_result != ESP_OK) {
    ESP_LOGE(TAG, "麦克风采集任务创建失败：%s",
             esp_err_to_name(monitor_result));
    return;
  }

  // 本地硬件就绪后立即开放页面和语音按键；联网功能随后初始化。
  atomic_store_explicit(&s_voice_ready, true, memory_order_release);
  ESP_LOGI(TAG, "系统已就绪：短按切换页面，长按开始语音对话");

  const esp_err_t network_result = app_network_init();
  if (network_result == ESP_OK) {
    // 天气由独立任务负责首次获取、失败重试和定时刷新。
    if (app_network_start_weather_updates() != ESP_OK) {
      ESP_LOGW(TAG, "天气自动更新任务启动失败");
    }
    if (app_network_sync_time() != ESP_OK) {
      ESP_LOGW(TAG, "Wi-Fi正常，但联网校时尚未成功");
    }
  } else {
    ESP_LOGW(TAG, "本次未连接Wi-Fi，跳过联网功能：%s",
             esp_err_to_name(network_result));
  }
}
