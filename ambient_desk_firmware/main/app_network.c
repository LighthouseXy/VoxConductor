#include "app_network.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "app_storage.h"
#include "app_ui.h"
#include "cJSON.h"
#include "esp_event.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "nvs_flash.h"
#include "wifi_credentials.h"

#define WIFI_MAX_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// 当前开发阶段使用Mac局域网地址，后续可迁移到持久化配置。
#define LOCAL_SERVER_WEATHER_URL "http://192.168.1.16:8000/weather/current"
#define AUDIO_TURN_URL "http://192.168.1.16:8000/audio/turn"
#define AUDIO_TURN_METADATA_URL_PREFIX "http://192.168.1.16:8000/audio/turn/"

#define DEVICE_SESSION_ID "ambient-desk-01"
#define TURN_METADATA_BUFFER_SIZE 4096
#define TTS_PLAYBACK_FRAMES_PER_BLOCK 256
// 约340ms音频，用来吸收HTTP和Wi-Fi短时抖动。
#define TTS_STREAM_PREBUFFER_BYTES (16 * 1024)
// 获取失败后30秒重试；成功后每10分钟更新一次。
#define WEATHER_RETRY_INTERVAL_MS 30000
#define WEATHER_REFRESH_INTERVAL_MS (10 * 60 * 1000)
#define WEATHER_TASK_STACK_SIZE 4096

static const char *TAG = "app_network";
static EventGroupHandle_t s_wifi_event_group;
static int s_wifi_retry_count;
static uint8_t s_tts_stream_prebuffer[TTS_STREAM_PREBUFFER_BYTES];

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
} http_response_buffer_t;

typedef struct {
  const char *expected_turn_id;
  bool turn_id_verified;
  size_t total_bytes;
  size_t prebuffer_length;
  uint8_t pending_byte;
  bool has_pending_byte;
  bool playback_started;
  esp_err_t playback_result;
} streaming_audio_context_t;

static esp_err_t init_nvs(void) {
  esp_err_t result = nvs_flash_init();
  if (result == ESP_ERR_NVS_NO_FREE_PAGES ||
      result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    result = nvs_flash_erase();
    if (result == ESP_OK) {
      result = nvs_flash_init();
    }
  }
  return result;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)argument;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    const esp_err_t result = esp_wifi_connect();
    if (result != ESP_OK) {
      ESP_LOGE(TAG, "启动Wi-Fi连接失败：%s", esp_err_to_name(result));
    }
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    app_ui_set_wifi_connected(false);

    if (s_wifi_retry_count < WIFI_MAX_RETRY) {
      ++s_wifi_retry_count;
      ESP_LOGW(TAG, "Wi-Fi已断开，正在重试：%d/%d", s_wifi_retry_count,
               WIFI_MAX_RETRY);
      const esp_err_t result = esp_wifi_connect();
      if (result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi重连启动失败：%s", esp_err_to_name(result));
      }
    } else {
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
    }
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Wi-Fi连接成功，IP地址：" IPSTR, IP2STR(&event->ip_info.ip));
    s_wifi_retry_count = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    app_ui_set_wifi_connected(true);
  }
}

esp_err_t app_network_init(void) {
  esp_err_t result = init_nvs();
  if (result != ESP_OK) {
    return result;
  }

  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    return ESP_ERR_NO_MEM;
  }

  if ((result = esp_netif_init()) != ESP_OK ||
      (result = esp_event_loop_create_default()) != ESP_OK) {
    return result;
  }
  if (esp_netif_create_default_wifi_sta() == NULL) {
    return ESP_FAIL;
  }

  const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  if ((result = esp_wifi_init(&init_config)) != ESP_OK ||
      (result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           wifi_event_handler, NULL)) != ESP_OK ||
      (result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           wifi_event_handler, NULL)) != ESP_OK) {
    return result;
  }

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
          },
  };

  if ((result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
      (result = esp_wifi_set_config(WIFI_IF_STA, &wifi_config)) != ESP_OK ||
      (result = esp_wifi_start()) != ESP_OK) {
    return result;
  }

  ESP_LOGI(TAG, "正在连接Wi-Fi：%s", WIFI_SSID);
  const EventBits_t bits = xEventGroupWaitBits(
      s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
      portMAX_DELAY);

  if ((bits & WIFI_CONNECTED_BIT) != 0) {
    return ESP_OK;
  }
  ESP_LOGE(TAG, "Wi-Fi连接失败：%s", WIFI_SSID);
  return ESP_FAIL;
}

bool app_network_is_connected(void) {
  return s_wifi_event_group != NULL &&
         (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static esp_err_t text_http_event_handler(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
      event->user_data == NULL) {
    return ESP_OK;
  }

  http_response_buffer_t *response = (http_response_buffer_t *)event->user_data;
  const size_t available = response->capacity - response->length - 1;
  size_t copy_length = (size_t)event->data_len;
  if (copy_length > available) {
    copy_length = available;
  }
  if (copy_length > 0) {
    memcpy(response->data + response->length, event->data, copy_length);
    response->length += copy_length;
    response->data[response->length] = '\0';
  }
  if (copy_length < (size_t)event->data_len) {
    ESP_LOGW(TAG, "HTTP文本响应超过缓冲区，后面的内容已截断");
  }
  return ESP_OK;
}

esp_err_t app_network_fetch_weather(void) {
  static char response_data[512];
  memset(response_data, 0, sizeof(response_data));

  http_response_buffer_t response = {
      .data = response_data,
      .capacity = sizeof(response_data),
  };

  const esp_http_client_config_t config = {
      .url = LOCAL_SERVER_WEATHER_URL,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 15000,
      .event_handler = text_http_event_handler,
      .user_data = &response,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }

  const esp_err_t request_result =
      esp_http_client_perform(client);
  const int status_code =
      esp_http_client_get_status_code(client);

  esp_http_client_cleanup(client);

  if (request_result != ESP_OK || status_code != 200) {
    return request_result != ESP_OK
               ? request_result
               : ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(response_data);
  const cJSON *city =
      cJSON_GetObjectItemCaseSensitive(root, "city");
  const cJSON *temperature =
      cJSON_GetObjectItemCaseSensitive(root, "temperature_c");
  const cJSON *apparent =
      cJSON_GetObjectItemCaseSensitive(root, "apparent_c");
  const cJSON *condition =
      cJSON_GetObjectItemCaseSensitive(root, "condition");
  const cJSON *weather_code =
      cJSON_GetObjectItemCaseSensitive(root, "weather_code");
  const cJSON *updated_at =
      cJSON_GetObjectItemCaseSensitive(root, "updated_at");

  if (!cJSON_IsString(city) || city->valuestring == NULL ||
      !cJSON_IsNumber(temperature) ||
      !cJSON_IsNumber(apparent) ||
      !cJSON_IsString(condition) ||
      condition->valuestring == NULL ||
      !cJSON_IsNumber(weather_code) ||
      !cJSON_IsString(updated_at) || updated_at->valuestring == NULL) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }

  app_ui_set_weather(
      city->valuestring,
      (int)temperature->valuedouble,
      (int)apparent->valuedouble,
      condition->valuestring,
      weather_code->valueint,
      updated_at->valuestring);

  ESP_LOGI(TAG, "天气更新：%s，%d°C，体感%d°C，%s",
      city->valuestring,
      (int)temperature->valuedouble,
      (int)apparent->valuedouble,
      condition->valuestring);

  cJSON_Delete(root);
  return ESP_OK;
}

static void weather_update_task(void *argument)
{
  (void)argument;

  while (true) {
    TickType_t next_delay =
        pdMS_TO_TICKS(WEATHER_RETRY_INTERVAL_MS);

    if (app_network_is_connected()) {
      const esp_err_t result = app_network_fetch_weather();

      if (result == ESP_OK) {
        // 请求成功后等待10分钟再刷新。
        next_delay =
            pdMS_TO_TICKS(WEATHER_REFRESH_INTERVAL_MS);
      } else {
        ESP_LOGW(TAG, "天气获取失败，30秒后重试：%s",
                 esp_err_to_name(result));
      }
    } else {
      ESP_LOGW(TAG, "网络未连接，暂缓获取天气");
    }

    vTaskDelay(next_delay);
  }
}

esp_err_t app_network_start_weather_updates(void)
{
  const BaseType_t result =
      xTaskCreate(weather_update_task,
                  "weather_update",
                  WEATHER_TASK_STACK_SIZE,
                  NULL,
                  4,
                  NULL);

  if (result != pdPASS) {
    ESP_LOGE(TAG, "创建天气更新任务失败");
    return ESP_ERR_NO_MEM;
  }

  return ESP_OK;
}

esp_err_t app_network_sync_time(void) {
  ESP_LOGI(TAG, "正在通过SNTP同步时间");
  const esp_sntp_config_t config =
      ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
  esp_err_t result = esp_netif_sntp_init(&config);
  if (result != ESP_OK) {
    return result;
  }
  result = esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000));
  if (result != ESP_OK) {
    return result;
  }

  setenv("TZ", "CST-8", 1);
  tzset();
  time_t now;
  struct tm local_time;
  char time_text[32];
  time(&now);
  localtime_r(&now, &local_time);
  strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", &local_time);
  ESP_LOGI(TAG, "当前北京时间：%s", time_text);
  return ESP_OK;
}

static esp_err_t play_stream_pcm_bytes(streaming_audio_context_t *context,
                                       const uint8_t *data, size_t length) {
  int16_t sample_block[TTS_PLAYBACK_FRAMES_PER_BLOCK];
  uint8_t *block_bytes = (uint8_t *)sample_block;
  size_t offset = 0;

  while (offset < length || context->has_pending_byte) {
    size_t block_length = 0;
    if (context->has_pending_byte) {
      block_bytes[block_length++] = context->pending_byte;
      context->has_pending_byte = false;
    }
    size_t copy_length = length - offset;
    const size_t available = sizeof(sample_block) - block_length;
    if (copy_length > available) {
      copy_length = available;
    }
    if (copy_length > 0) {
      memcpy(block_bytes + block_length, data + offset, copy_length);
      block_length += copy_length;
      offset += copy_length;
    }
    if ((block_length % sizeof(int16_t)) != 0) {
      context->pending_byte = block_bytes[block_length - 1];
      context->has_pending_byte = true;
      --block_length;
    }
    if (block_length == 0) {
      break;
    }
    const esp_err_t result = app_audio_play_mono_pcm(
        sample_block, block_length / sizeof(int16_t));
    if (result != ESP_OK) {
      return result;
    }
  }
  return ESP_OK;
}

static esp_err_t streaming_http_event_handler(esp_http_client_event_t *event) {
  streaming_audio_context_t *context =
      (streaming_audio_context_t *)event->user_data;

  if (event->event_id == HTTP_EVENT_ON_HEADER && context != NULL &&
      event->header_key != NULL && event->header_value != NULL &&
      strcasecmp(event->header_key, "X-Turn-ID") == 0) {
    if (strcmp(event->header_value, context->expected_turn_id) != 0) {
      ESP_LOGE(TAG, "收到错误轮次：期望%s，实际%s", context->expected_turn_id,
               event->header_value);
      return ESP_ERR_INVALID_RESPONSE;
    }
    context->turn_id_verified = true;
    return ESP_OK;
  }

  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
      context == NULL) {
    return ESP_OK;
  }
  if (esp_http_client_get_status_code(event->client) != 200) {
    return ESP_OK;
  }
  if (!context->turn_id_verified) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  const uint8_t *data = (const uint8_t *)event->data;
  size_t length = (size_t)event->data_len;
  context->total_bytes += length;

  if (!context->playback_started) {
    const size_t remaining =
        TTS_STREAM_PREBUFFER_BYTES - context->prebuffer_length;
    const size_t copy_length = length < remaining ? length : remaining;
    memcpy(s_tts_stream_prebuffer + context->prebuffer_length, data,
           copy_length);
    context->prebuffer_length += copy_length;
    data += copy_length;
    length -= copy_length;
    if (context->prebuffer_length < TTS_STREAM_PREBUFFER_BYTES) {
      return ESP_OK;
    }

    context->playback_started = true;
    app_ui_show_voice_state(APP_UI_VOICE_SPEAKING, "正在播放回答");
    context->playback_result = play_stream_pcm_bytes(
        context, s_tts_stream_prebuffer, context->prebuffer_length);
    context->prebuffer_length = 0;
    if (context->playback_result != ESP_OK) {
      return context->playback_result;
    }
  }

  if (length > 0) {
    context->playback_result = play_stream_pcm_bytes(context, data, length);
  }
  return context->playback_result;
}

esp_err_t app_network_upload_voice_turn(const int16_t *recording,
                                        size_t recording_bytes,
                                        const char *turn_id) {
  if (recording == NULL || recording_bytes == 0 || turn_id == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  streaming_audio_context_t stream = {
      .expected_turn_id = turn_id,
      .playback_result = ESP_OK,
  };
  const esp_http_client_config_t config = {
      .url = AUDIO_TURN_URL,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 180000,
      .event_handler = streaming_http_event_handler,
      .user_data = &stream,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }

  esp_http_client_set_header(client, "Content-Type",
                             "application/octet-stream");
  esp_http_client_set_header(client, "X-Session-ID", DEVICE_SESSION_ID);
  esp_http_client_set_header(client, "X-Turn-ID", turn_id);
  esp_http_client_set_post_field(client, (const char *)recording,
                                 (int)recording_bytes);
  ESP_LOGI(TAG, "正在上传语音：session=%s，turn=%s，%u字节",
           DEVICE_SESSION_ID, turn_id, (unsigned)recording_bytes);

  const esp_err_t request_result = esp_http_client_perform(client);
  const int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (request_result != ESP_OK || status_code != 200) {
    return request_result != ESP_OK ? request_result : ESP_FAIL;
  }

  // 极短回答不足预缓冲大小时，在HTTP结束后播放剩余音频。
  if (!stream.playback_started && stream.prebuffer_length > 0) {
    stream.playback_started = true;
    app_ui_show_voice_state(APP_UI_VOICE_SPEAKING, "正在播放回答");
    stream.playback_result = play_stream_pcm_bytes(
        &stream, s_tts_stream_prebuffer, stream.prebuffer_length);
  }
  if (stream.playback_result != ESP_OK) {
    return stream.playback_result;
  }
  if (stream.total_bytes == 0 || stream.has_pending_byte) {
    return ESP_ERR_INVALID_SIZE;
  }
  ESP_LOGI(TAG, "AI流式播放完成：%u字节", (unsigned)stream.total_bytes);
  return ESP_OK;
}

esp_err_t app_network_fetch_and_store_turn(const char *turn_id) {
  if (turn_id == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  char turn_url[192];
  const int url_length = snprintf(turn_url, sizeof(turn_url), "%s%s",
                                  AUDIO_TURN_METADATA_URL_PREFIX, turn_id);
  if (url_length < 0 || (size_t)url_length >= sizeof(turn_url)) {
    return ESP_ERR_INVALID_SIZE;
  }

  static char response_data[TURN_METADATA_BUFFER_SIZE];
  memset(response_data, 0, sizeof(response_data));
  http_response_buffer_t response = {
      .data = response_data,
      .capacity = sizeof(response_data),
  };
  const esp_http_client_config_t config = {
      .url = turn_url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 15000,
      .event_handler = text_http_event_handler,
      .user_data = &response,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }
  esp_http_client_set_header(client, "X-Session-ID", DEVICE_SESSION_ID);
  const esp_err_t request_result = esp_http_client_perform(client);
  const int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (request_result != ESP_OK || status_code != 200) {
    return request_result != ESP_OK ? request_result : ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(response_data);
  const cJSON *transcript = root != NULL
                                ? cJSON_GetObjectItemCaseSensitive(
                                      root, "transcript")
                                : NULL;
  const cJSON *answer =
      root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "answer") : NULL;
  if (!cJSON_IsString(transcript) || transcript->valuestring == NULL ||
      !cJSON_IsString(answer) || answer->valuestring == NULL) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }
  const esp_err_t result = app_storage_append_conversation(
      transcript->valuestring, answer->valuestring);
  cJSON_Delete(root);
  return result;
}
