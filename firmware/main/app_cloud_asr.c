/*
 * 实时识别适配：采集与WSS上传并行，结束时手动commit再finish。
 * available以release/acquire发布连续有效PCM，不能读未填写的采样。
 * PCM活到采集任务退出且WSS回调停止；返回后调用者才能释放。
 * 续问无声等待留在本地，NOT_FOUND是安静超时，不写空历史。
 */
#include "app_cloud.h"
#include "app_log.h"
#include "app_memory_alloc.h"
#include "app_audio.h"
#include "app_ui.h"
#include "app_cloud_wire.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#if __has_include("cloud_credentials.h")
#include "cloud_credentials.h"
#else
#include "cloud_credentials.example.h"
#endif

#ifndef CLOUD_ASR_REALTIME_URL
#define CLOUD_ASR_REALTIME_URL "wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3-asr-flash-realtime"
#endif
#define ASR_CREATED BIT0
#define ASR_UPDATED BIT1
#define ASR_FINISHED BIT2
#define CAPTURE_DONE BIT3
#define ASR_ERROR BIT4
static const char *TAG = "asr";

typedef struct {
  EventGroupHandle_t events;
  int16_t *pcm;
  _Atomic size_t available;
  _Atomic int error;
  size_t captured;
  int capture_result;
  bool followup;
  int32_t capture_end_us;
  int64_t start_us, deadline_us;
  char *message;
  cloud_ws_message_t assembly;
  char transcript[2048];
} asr_session_t;

static void capture_progress(size_t samples, void *context) {
  asr_session_t *s = context;
  atomic_store_explicit(&s->available, samples, memory_order_release);
}
static void capture_task(void *context) {
  asr_session_t *s = context;
  int result = app_audio_capture_progress(s->pcm, &s->captured, capture_progress,
                                         s, s->followup);
  s->capture_result = result;
  if (result != ESP_OK) atomic_store(&s->error, result);
  s->capture_end_us = (int32_t)(esp_timer_get_time() - s->start_us);
  xEventGroupSetBits(s->events, CAPTURE_DONE);
  vTaskDelete(NULL);
}
static void asr_event(void *context, esp_event_base_t base, int32_t id, void *data) {
  (void)base;
  asr_session_t *s = context;
  esp_websocket_event_data_t *e = data;
  if (id == WEBSOCKET_EVENT_ERROR || id == WEBSOCKET_EVENT_DISCONNECTED) {
    if (!(xEventGroupGetBits(s->events) & ASR_FINISHED)) {
      atomic_store(&s->error, ESP_FAIL); xEventGroupSetBits(s->events, ASR_ERROR);
    }
    return;
  }
  if (id != WEBSOCKET_EVENT_DATA || e->op_code >= 8) return;
  int result = cloud_ws_append(&s->assembly, s->message, 8192, e->op_code, e->fin,
      e->payload_len, e->payload_offset, e->data_ptr, e->data_len);
  if (result == 0) return;
  cJSON *r = result > 0 ? cJSON_Parse(s->message) : NULL;
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(r, "type");
  bool ok = cJSON_IsString(type);
  if (ok && strcmp(type->valuestring, "session.created") == 0) xEventGroupSetBits(s->events, ASR_CREATED);
  else if (ok && strcmp(type->valuestring, "session.updated") == 0) xEventGroupSetBits(s->events, ASR_UPDATED);
  else if (ok && strcmp(type->valuestring, "session.finished") == 0) xEventGroupSetBits(s->events, ASR_FINISHED);
  else if (ok && strcmp(type->valuestring, "conversation.item.input_audio_transcription.completed") == 0) {
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(r, "transcript");
    ok = cJSON_IsString(text) && strlen(text->valuestring) + strlen(s->transcript) < sizeof(s->transcript);
    if (ok) strcat(s->transcript, text->valuestring);
  } else if (ok && (strcmp(type->valuestring, "error") == 0 ||
                   strcmp(type->valuestring, "conversation.item.input_audio_transcription.failed") == 0)) {
    const cJSON *error = cJSON_GetObjectItemCaseSensitive(r, "error");
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(error, "code");
    ESP_LOGW(TAG, "ASR server error: type=%s, code=%.80s", type->valuestring,
             cJSON_IsString(code) ? code->valuestring : "unknown");
    ok = false;
  }
  if (!ok) { atomic_store(&s->error, ESP_ERR_INVALID_RESPONSE); xEventGroupSetBits(s->events, ASR_ERROR); }
  cJSON_Delete(r);
}
static esp_err_t wait_asr(asr_session_t *s, EventBits_t wanted) {
  for (int i = 0; i < 150; ++i) {
    if (s->deadline_us > 0 && esp_timer_get_time() >= s->deadline_us) return ESP_ERR_TIMEOUT;
    if (app_cloud_is_cancelled()) return ESP_ERR_INVALID_STATE;
    if (atomic_load(&s->error) != ESP_OK) return atomic_load(&s->error);
    if (xEventGroupWaitBits(s->events, wanted | ASR_ERROR, pdFALSE, pdFALSE, pdMS_TO_TICKS(100)) & wanted) return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}
static bool send_json(esp_websocket_client_handle_t c, const asr_session_t *s, const char *json) {
  int64_t left = s->deadline_us > 0 ? (s->deadline_us - esp_timer_get_time()) / 1000 : 5000;
  if (left <= 0) return false;
  unsigned timeout = left < 5000 ? (unsigned)left : 5000;
  return esp_websocket_client_send_text(c, json, strlen(json), pdMS_TO_TICKS(timeout)) == (int)strlen(json);
}

esp_err_t app_cloud_listen(int16_t *pcm, size_t *samples, char **text,
                           app_turn_t *turn, bool followup) {
  if (!pcm || !samples || !text || !turn || time(NULL) < 1704067200 ||
      strncmp(CLOUD_ASR_REALTIME_URL, "wss://", 6) != 0 ||
      strlen(CLOUD_DASHSCOPE_API_KEY) > 256 || !*CLOUD_DASHSCOPE_API_KEY) return ESP_ERR_INVALID_ARG;
  *text = NULL; *samples = 0;
  asr_session_t *s = app_psram_zero(sizeof(*s));
  if (!s) return ESP_ERR_NO_MEM;
  s->deadline_us = turn->deadline_us;
  s->message = app_psram_malloc(8193);
  if (!s->message) { free(s); return ESP_ERR_NO_MEM; }
  s->events = xEventGroupCreate(); s->pcm = pcm; s->start_us = esp_timer_get_time();
  atomic_init(&s->available, 0); atomic_init(&s->error, ESP_OK);
  s->followup = followup;
  if (!s->events) { free(s->message); free(s); return ESP_ERR_NO_MEM; }
  if (!followup) app_turn_emit(turn, TURN_RECORD_START, NULL, ESP_OK);
  bool capturing = xTaskCreate(capture_task, "asr_capture", 4096, s, 7, NULL) == pdPASS;
  esp_err_t result = capturing ? ESP_OK : ESP_ERR_NO_MEM;
  const char *stage = followup ? "followup_wait" : "connect";
  app_cloud_summary_interrupt();
  // Capture is already running; finish the summary TLS session before ASR TLS.
  int64_t summary_wait_start = esp_timer_get_time();
  int64_t summary_deadline = esp_timer_get_time() + 15000000;
  while (capturing && app_cloud_summary_busy() && !app_cloud_is_cancelled()) {
    if (atomic_load(&s->error) != ESP_OK) { result = atomic_load(&s->error); break; }
    if (esp_timer_get_time() >= summary_deadline || app_turn_expired(turn, esp_timer_get_time())) {
      stage = "summary_cleanup"; result = ESP_ERR_TIMEOUT; break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  turn->summary_wait_us = esp_timer_get_time() - summary_wait_start;
  // The same capture/VAD task keeps local pre-roll. No idle cloud connection,
  // empty recognition request or conversation-history entry for quiet expiry.
  while (followup && result == ESP_OK &&
         !atomic_load_explicit(&s->available, memory_order_acquire)) {
    if (app_turn_expired(turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; break; }
    if (app_cloud_is_cancelled()) { result = ESP_ERR_INVALID_STATE; break; }
    if (atomic_load(&s->error) != ESP_OK) { result = atomic_load(&s->error); break; }
    if (xEventGroupGetBits(s->events) & CAPTURE_DONE) {
      // The final progress publication can race the first available read.
      if (atomic_load_explicit(&s->available, memory_order_acquire)) break;
      result = atomic_load(&s->error);
      if (result == ESP_OK) result = ESP_ERR_NOT_FOUND;
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
  if (app_cloud_is_cancelled()) result = ESP_ERR_INVALID_STATE;
  if (result == ESP_OK) {
    stage = "connect";
    if (followup) app_turn_emit_at(turn, TURN_RECORD_START, s->start_us);
    app_turn_emit(turn, TURN_ASR_START, NULL, ESP_OK);
  }
  char header[280]; snprintf(header, sizeof(header), "Authorization: Bearer %s\r\n", CLOUD_DASHSCOPE_API_KEY);
  esp_websocket_client_config_t config = {.uri = CLOUD_ASR_REALTIME_URL, .headers = header,
    .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_reconnect = true,
    .task_stack = 8192, .buffer_size = 2048, .network_timeout_ms = app_turn_remaining_ms(turn, esp_timer_get_time(), 10000)};
  if (result == ESP_OK && app_turn_expired(turn, esp_timer_get_time())) result = ESP_ERR_TIMEOUT;
  esp_websocket_client_handle_t client = result == ESP_OK ? esp_websocket_client_init(&config) : NULL;
  bool started = false;
  if (result == ESP_OK && !client) result = ESP_ERR_NO_MEM;
  if (result == ESP_OK) result = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, asr_event, s);
  if (result == ESP_OK) { result = esp_websocket_client_start(client); started = result == ESP_OK; }
  if (result == ESP_OK) result = wait_asr(s, ASR_CREATED);
  int64_t connected_us = esp_timer_get_time();
  if (result == ESP_OK) stage = "configure";
  if (result == ESP_OK && !send_json(client, s,
      "{\"event_id\":\"config-1\",\"type\":\"session.update\",\"session\":{\"input_audio_format\":\"pcm\",\"sample_rate\":16000,\"input_audio_transcription\":{\"language\":\"zh\"},\"turn_detection\":null}}")) result = ESP_FAIL;
  if (result == ESP_OK) result = wait_asr(s, ASR_UPDATED);
  int64_t configured_us = esp_timer_get_time();
  size_t sent = 0; unsigned sequence = 1;
  char *encoded = result == ESP_OK ? app_psram_malloc(4097) : NULL;
  char *json = result == ESP_OK ? app_psram_malloc(4300) : NULL;
  if (result == ESP_OK && (!encoded || !json)) result = ESP_ERR_NO_MEM;
  if (result == ESP_OK) stage = "audio_upload";
  while (result == ESP_OK) {
    if (app_turn_expired(turn, esp_timer_get_time())) { result = ESP_ERR_TIMEOUT; break; }
    if (app_cloud_is_cancelled()) { result = ESP_ERR_INVALID_STATE; break; }
    if (atomic_load(&s->error) != ESP_OK) { result = atomic_load(&s->error); break; }
    if (esp_timer_get_time() - s->start_us > 60000000) { result = ESP_ERR_TIMEOUT; break; }
    size_t available = atomic_load_explicit(&s->available, memory_order_acquire);
    if (sent < available) {
      size_t n = available - sent; if (n > 1536) n = 1536;
      size_t length = 0;
      if (mbedtls_base64_encode((unsigned char *)encoded, 4097, &length,
                               (const unsigned char *)(pcm + sent), n * 2) != 0) { result = ESP_FAIL; break; }
      encoded[length] = 0;
      snprintf(json, 4300, "{\"event_id\":\"audio-%u\",\"type\":\"input_audio_buffer.append\",\"audio\":\"%s\"}", ++sequence, encoded);
      if (!send_json(client, s, json)) { result = ESP_FAIL; break; }
      turn->asr_request_bytes += strlen(json); sent += n;
    } else if ((xEventGroupGetBits(s->events) & CAPTURE_DONE) &&
               sent == atomic_load_explicit(&s->available, memory_order_acquire)) break;
    else vTaskDelay(pdMS_TO_TICKS(10));
  }
  free(encoded); free(json);
  if (result == ESP_OK && atomic_load(&s->error) != ESP_OK) result = atomic_load(&s->error);
  if (app_cloud_is_cancelled()) result = ESP_ERR_INVALID_STATE;
  if (result == ESP_OK && app_turn_expired(turn, esp_timer_get_time())) result = ESP_ERR_TIMEOUT;
  if (result == ESP_OK) app_ui_show_voice_state(APP_UI_VOICE_THINKING, "正在处理语音");
  // turn_detection=null is manual ASR mode, even when capture was wake-triggered.
  // append only buffers audio: commit must precede finish to trigger recognition.
  if (result == ESP_OK) {
    stage = "commit";
    if (!sent) result = ESP_ERR_INVALID_RESPONSE;
    else if (!send_json(client, s, "{\"event_id\":\"commit\",\"type\":\"input_audio_buffer.commit\"}")) result = ESP_FAIL;
    else APP_LOG_DETAIL(TAG, "ASR submitted: samples=%u, duration=%ums",
                  (unsigned)sent, (unsigned)(sent * 1000 / APP_AUDIO_SAMPLE_RATE_HZ));
  }
  if (result == ESP_OK) stage = "recognition_result";
  int64_t submitted_us = esp_timer_get_time();
  if (result == ESP_OK && !send_json(client, s, "{\"event_id\":\"finish\",\"type\":\"session.finish\"}")) result = ESP_FAIL;
  if (result == ESP_OK) result = wait_asr(s, ASR_FINISHED);
  int64_t recognized_us = esp_timer_get_time();
  if (capturing) {
    if (!(xEventGroupGetBits(s->events) & CAPTURE_DONE)) app_audio_request_record_stop();
    // 采集任务持有pcm指针，必须退出后才能释放；I2S读取有1秒超时。
    xEventGroupWaitBits(s->events, CAPTURE_DONE, pdFALSE, pdTRUE, portMAX_DELAY);
    *samples = s->captured;
    if (s->capture_result == ESP_OK)
      app_turn_emit_at(turn, TURN_RECORD_END, s->start_us + s->capture_end_us);
  }
  if (client) { if (started) esp_websocket_client_stop(client); esp_websocket_client_destroy(client); }
  if (result == ESP_OK && s->transcript[0]) { *text = strdup(s->transcript); if (!*text) result = ESP_ERR_NO_MEM; }
  else if (result == ESP_OK) result = ESP_ERR_INVALID_RESPONSE;
  if (followup && result == ESP_ERR_NOT_FOUND)
    APP_LOG_DETAIL(TAG, "Follow-up idle expiry: no cloud request sent");
  else if (app_cloud_is_cancelled())
    APP_LOG_DETAIL(TAG, "[%s] ASR cancelled: stage=%s", app_log_id(turn->id), stage);
  else if (result != ESP_OK)
    ESP_LOGW(TAG, "[%s] ASR failed: stage=%s, status=%s, capture=%s, captured=%u, sent=%u",
             app_log_id(turn->id),
             capturing && s->capture_result != ESP_OK ? "capture" : stage,
             esp_err_to_name(result),
             capturing ? esp_err_to_name(s->capture_result) : "not_started",
             (unsigned)s->captured, (unsigned)sent);
  if (sent) APP_LOG_DETAIL(TAG, "ASR timing: connect=%lldms configure=%lldms capture_end=%dms submit=%lldms result_wait=%lldms cleanup=%lldms",
      (long long)((connected_us - s->start_us) / 1000),
      (long long)((configured_us - connected_us) / 1000), s->capture_end_us / 1000,
      (long long)((submitted_us - s->start_us) / 1000),
      (long long)((recognized_us - submitted_us) / 1000),
      (long long)((esp_timer_get_time() - recognized_us) / 1000));
  vEventGroupDelete(s->events); free(s->message); free(s);
  return result;
}
