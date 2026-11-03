/*
 * TTS适配：网络回调解码到有界PSRAM队列，播放任务消费PCM。
 * 一次会话只快照一次语速/音色，不中途改格式。首播预缓冲和断粮等待分开计量。
 * 取消后先结束任务/回调再释放缓冲；提交I2S不等于声学上已听到。
 */
#include "app_cloud.h"
#include "app_log.h"
#include "app_acoustic.h"
#include "app_barge_diagnostic.h"
#include "app_tts_options.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app_audio.h"
#include "app_cloud_wire.h"
#include "app_ui.h"
#include "app_network.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"

#if defined(APP_TTS_HOST_TEST)
#include <cloud_credentials.h> // 测试使用合成凭据和模拟网络，不读取私有配置。
#elif __has_include("cloud_credentials.h")
#include "cloud_credentials.h"
#else
#include "cloud_credentials.example.h"
#endif

#define WS_MESSAGE_LIMIT (64 * 1024)
#define PCM_QUEUE_BYTES (384 * 1024)
#define PCM_PREBUFFER_BYTES (24 * 1024) // 24k/16bit单声道约0.51秒，容量仍有界。
#define TTS_PLAY_PRIORITY 5
#define TTS_OUTPUT_PRIORITY 6
#define TTS_SESSION_LIMIT_US 360000000 // 详细回答可超过两分钟；整轮最多六分钟，断流仍限30秒。
#define CREATED_BIT BIT0
#define UPDATED_BIT BIT1
#define ERROR_BIT BIT2

static const char *TAG = "app_cloud_tts";

typedef struct {
  EventGroupHandle_t events;
  StreamBufferHandle_t pcm;
  StaticStreamBuffer_t pcm_control;
  uint8_t *pcm_storage;
  char *message;
  uint8_t *decoded;
  cloud_ws_message_t assembly;
  _Atomic bool finished;
  _Atomic int error;
  _Atomic int32_t first_pcm_elapsed_us;
  size_t received_bytes;
  int64_t start_us, deadline_us;
  const _Atomic bool *abort;
  app_tts_feed_t feed;
  void *feed_context;
  const char *fixed_text;
  esp_websocket_client_handle_t client;
  _Atomic bool input_ready, input_finished, input_done;
  _Atomic unsigned text_chunks;
  app_tts_options_t options;
} tts_stream_t;

const char *app_cloud_tts_voice(unsigned id) {
  if (id >= APP_TTS_VOICE_COUNT) return NULL;
  return id ? app_tts_voices[id].voice : CLOUD_TTS_VOICE;
}

static bool stopped(const tts_stream_t *stream) {
  return (stream->deadline_us > 0 && esp_timer_get_time() >= stream->deadline_us) ||
      app_cloud_is_cancelled() || (stream->abort && atomic_load(stream->abort));
}

static void fail_stream(tts_stream_t *stream, esp_err_t error) {
  int expected = ESP_OK;
  atomic_compare_exchange_strong(&stream->error, &expected, error);
  xEventGroupSetBits(stream->events, ERROR_BIT);
}

static void process_message(tts_stream_t *stream) {
  if (stopped(stream)) { fail_stream(stream, ESP_ERR_INVALID_STATE); return; }
  cJSON *root = cJSON_Parse(stream->message);
  const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
  if (!cJSON_IsString(type)) {
    fail_stream(stream, ESP_ERR_INVALID_RESPONSE);
  } else if (strcmp(type->valuestring, "session.created") == 0) {
    xEventGroupSetBits(stream->events, CREATED_BIT);
  } else if (strcmp(type->valuestring, "session.updated") == 0) {
    const cJSON *session = cJSON_GetObjectItemCaseSensitive(root, "session");
    const cJSON *rate = cJSON_GetObjectItemCaseSensitive(session, "sample_rate");
    const cJSON *format = cJSON_GetObjectItemCaseSensitive(session, "response_format");
    if (!cJSON_IsNumber(rate) || rate->valueint != 24000 ||
        !cJSON_IsString(format) || strcmp(format->valuestring, "pcm") != 0)
      fail_stream(stream, ESP_ERR_INVALID_RESPONSE);
    else xEventGroupSetBits(stream->events, UPDATED_BIT);
  } else if (strcmp(type->valuestring, "response.audio.delta") == 0) {
    const cJSON *delta = cJSON_GetObjectItemCaseSensitive(root, "delta");
    size_t decoded_bytes = 0;
    if (atomic_load(&stream->finished) || !cJSON_IsString(delta) ||
        mbedtls_base64_decode(stream->decoded, WS_MESSAGE_LIMIT, &decoded_bytes,
                              (const unsigned char *)delta->valuestring,
                              strlen(delta->valuestring)) != 0) {
      fail_stream(stream, ESP_ERR_INVALID_RESPONSE);
    } else if (decoded_bytes) {
      if (!stream->received_bytes)
        atomic_store(&stream->first_pcm_elapsed_us,
                     (int32_t)(esp_timer_get_time() - stream->start_us));
      // 回调只负责入队，播放由调用任务完成。缓冲耗尽则明确失败，不丢音频。
      size_t written = 0;
      int64_t deadline = esp_timer_get_time() + 2000000;
      while (written < decoded_bytes && esp_timer_get_time() < deadline &&
             !stopped(stream) && atomic_load(&stream->error) == ESP_OK) {
        written += xStreamBufferSend(stream->pcm, stream->decoded + written,
                                    decoded_bytes - written, pdMS_TO_TICKS(20));
      }
      if (written != decoded_bytes) fail_stream(stream, ESP_ERR_TIMEOUT);
      stream->received_bytes += written;
    }
  } else if (strcmp(type->valuestring, "session.finished") == 0) {
    if (!stream->received_bytes) fail_stream(stream, ESP_ERR_INVALID_SIZE);
    atomic_store(&stream->finished, true);
  } else if (strcmp(type->valuestring, "error") == 0) {
    ESP_LOGE(TAG, "TTS返回错误事件，请检查模型、音色、地域和额度");
    fail_stream(stream, ESP_FAIL);
  } else if (strcmp(type->valuestring, "response.done") == 0) {
    const cJSON *response = cJSON_GetObjectItemCaseSensitive(root, "response");
    const cJSON *status = cJSON_GetObjectItemCaseSensitive(response, "status");
    if (cJSON_IsString(status) && strcmp(status->valuestring, "completed") != 0)
      fail_stream(stream, ESP_FAIL);
  }
  cJSON_Delete(root);
}

static void websocket_event(void *argument, esp_event_base_t base,
                              int32_t event_id, void *event_data) {
  (void)base;
  tts_stream_t *stream = argument;
  esp_websocket_event_data_t *event = event_data;
  if (event_id == WEBSOCKET_EVENT_ERROR || event_id == WEBSOCKET_EVENT_DISCONNECTED ||
      event_id == WEBSOCKET_EVENT_CLOSED) {
    if (!atomic_load(&stream->finished)) {
      if (event_id == WEBSOCKET_EVENT_ERROR && event)
        ESP_LOGE(TAG, "WSS错误：握手HTTP=%d，TLS=0x%x",
                 event->error_handle.esp_ws_handshake_status_code,
                 (unsigned)event->error_handle.esp_tls_last_esp_err);
      fail_stream(stream, ESP_FAIL);
    }
    return;
  }
  if (event_id != WEBSOCKET_EVENT_DATA || atomic_load(&stream->error) != ESP_OK)
    return;
  if (event->op_code >= 8) return;  // CLOSE/PING/PONG由WebSocket组件处理。
  int result = cloud_ws_append(&stream->assembly, stream->message, WS_MESSAGE_LIMIT,
                                event->op_code, event->fin, event->payload_len,
                                event->payload_offset, event->data_ptr, event->data_len);
  if (result < 0) fail_stream(stream, ESP_ERR_INVALID_SIZE);
  else if (result == 1) process_message(stream);
}

static esp_err_t wait_event(tts_stream_t *stream, EventBits_t wanted) {
  for (int i = 0; i < 150; ++i) {
    if (stopped(stream)) return ESP_ERR_INVALID_STATE;
    EventBits_t bits = xEventGroupWaitBits(stream->events, wanted | ERROR_BIT,
                                           pdFALSE, pdFALSE, pdMS_TO_TICKS(100));
    if (bits & ERROR_BIT) return atomic_load(&stream->error);
    if (bits & wanted) return ESP_OK;
  }
  return ESP_ERR_TIMEOUT;
}

static esp_err_t send_event(tts_stream_t *stream, const char *type,
                            unsigned sequence, const char *text) {
  cJSON *root = cJSON_CreateObject();
  if (!root) return ESP_ERR_NO_MEM;
  char id[48];
  snprintf(id, sizeof(id), "esp-%lld-%u", (long long)esp_timer_get_time(), sequence);
  bool ok = cJSON_AddStringToObject(root, "event_id", id) &&
            cJSON_AddStringToObject(root, "type", type);
  if (text) ok = ok && cJSON_AddStringToObject(root, "text", text);
  if (strcmp(type, "session.update") == 0) {
    cJSON *session = cJSON_AddObjectToObject(root, "session");
    ok = ok && session && cJSON_AddStringToObject(session, "voice", app_cloud_tts_voice(stream->options.voice_id)) &&
         cJSON_AddStringToObject(session, "mode", "server_commit") &&
         cJSON_AddStringToObject(session, "language_type", "Chinese") &&
         cJSON_AddStringToObject(session, "response_format", "pcm") &&
         cJSON_AddNumberToObject(session, "sample_rate", 24000) &&
         cJSON_AddNumberToObject(session, "speech_rate", stream->options.rate_percent / 100.0);
  }
  char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
  cJSON_Delete(root);
  if (!json) return ESP_ERR_NO_MEM;
  int length = strlen(json);
  int64_t left = stream->deadline_us > 0 ? (stream->deadline_us - esp_timer_get_time()) / 1000 : 10000;
  if (left <= 0) { free(json); return ESP_ERR_TIMEOUT; }
  unsigned timeout = left < 10000 ? (unsigned)left : 10000;
  int sent = esp_websocket_client_send_text(stream->client, json, length, pdMS_TO_TICKS(timeout));
  free(json);
  return sent == length ? ESP_OK : ESP_FAIL;
}

static void tts_input_task(void *argument) {
  tts_stream_t *stream = argument;
  unsigned sequence = 2;
  esp_err_t result = ESP_OK;
  // Reserve this existing task's internal stack before TLS fragments the heap.
  // Sending still waits for session.updated; failure/cancel releases the gate.
  while (!atomic_load(&stream->input_ready) && !stopped(stream) &&
         atomic_load(&stream->error) == ESP_OK)
    vTaskDelay(pdMS_TO_TICKS(10));
  result = atomic_load(&stream->error);
  while (!stopped(stream) && atomic_load(&stream->error) == ESP_OK) {
    char next[481] = {0};
    bool final = !stream->feed;
    const char *text = stream->fixed_text;
    if (stream->feed) {
      result = stream->feed(stream->feed_context, next, &final);
      text = next;
      if (result != ESP_OK) break;
    }
    if (stopped(stream) || atomic_load(&stream->error) != ESP_OK) break;
    if (text && *text) {
      result = send_event(stream, "input_text_buffer.append", sequence++, text);
      if (result != ESP_OK) break;
      atomic_fetch_add(&stream->text_chunks, 1);
    }
    if (stopped(stream) || atomic_load(&stream->error) != ESP_OK) break;
    if (final) {
      // 先标记再发送，避免很快到达的session.finished被误判为提前关闭。
      atomic_store(&stream->input_finished, true);
      result = send_event(stream, "session.finish", sequence++, NULL);
      break;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  if (stopped(stream)) result = ESP_ERR_INVALID_STATE;
  if (result != ESP_OK) fail_stream(stream, result);
  // 最后一次访问stream；调用者见到done后才可释放回调/输入共享状态。
  atomic_store(&stream->input_done, true);
  vTaskDelete(NULL);
}

static void report_first_pcm(const tts_stream_t *stream, app_turn_t *turn) {
  int32_t elapsed = atomic_load(&stream->first_pcm_elapsed_us);
  if (elapsed >= 0 && turn->times[TURN_FIRST_PCM] < 0)
    app_turn_emit_at(turn, TURN_FIRST_PCM, stream->start_us + elapsed);
}

static esp_err_t speak(const char *text, app_tts_feed_t feed, void *context,
                       app_turn_t *turn, bool reminder, const _Atomic bool *abort) {
  if (!turn || (!feed && (!text || !*text)) || !*CLOUD_DASHSCOPE_API_KEY ||
      strlen(CLOUD_DASHSCOPE_API_KEY) > 256 ||
      strncmp(CLOUD_TTS_URL, "wss://", 6) != 0) return ESP_ERR_INVALID_ARG;
  if (app_turn_expired(turn, esp_timer_get_time())) return ESP_ERR_TIMEOUT;
  if (time(NULL) < 1704067200) return ESP_ERR_INVALID_STATE;
  if (app_cloud_is_cancelled() || (abort && atomic_load(abort))) return ESP_ERR_INVALID_STATE;
  app_tts_options_t options;
  if (!app_desktop_tts_options(&options)) return ESP_ERR_INVALID_STATE;
  // Resolve before reserving WSS/sender stacks. The connection still uses the
  // original hostname and certificate verification; no pinned IP or HTTP fallback.
  esp_err_t resolved = app_network_resolve_url(CLOUD_TTS_URL);
  if (resolved != ESP_OK) return resolved;
  if (app_cloud_is_cancelled() || (abort && atomic_load(abort))) return ESP_ERR_INVALID_STATE;
  tts_stream_t *stream = calloc(1, sizeof(*stream));
  if (!stream) return ESP_ERR_NO_MEM;
  stream->options = options;
  ++turn->tts_batches;
  stream->abort = abort;
  atomic_init(&stream->error, ESP_OK);
  atomic_init(&stream->finished, false);
  atomic_init(&stream->first_pcm_elapsed_us, -1);
  atomic_init(&stream->input_ready, false);
  atomic_init(&stream->input_finished, false);
  atomic_init(&stream->input_done, true);
  atomic_init(&stream->text_chunks, 0);
  stream->feed = feed;
  stream->feed_context = context;
  stream->fixed_text = text;
  stream->start_us = esp_timer_get_time();
  stream->deadline_us = turn->deadline_us;
  stream->events = xEventGroupCreate();
  stream->pcm_storage = heap_caps_malloc(PCM_QUEUE_BYTES + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  stream->message = heap_caps_malloc(WS_MESSAGE_LIMIT + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  stream->decoded = heap_caps_malloc(WS_MESSAGE_LIMIT, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
  esp_websocket_client_handle_t client = NULL;
  bool started = false;
  esp_err_t result = ESP_ERR_NO_MEM;
  const char *stage = "buffers";
  uint32_t peak = 0;
  uint64_t absolute_sum = 0;
  size_t sample_total = 0;
  size_t prebuffer_bytes = PCM_PREBUFFER_BYTES;
  int64_t starving_since = 0;
  UBaseType_t output_priority = uxTaskPriorityGet(NULL);
  bool output_boosted = false;
  int64_t last_write_end = 0, max_feed_gap_us = 0, max_write_us = 0;
  size_t min_pcm_available = SIZE_MAX;
  if (!stream->events || !stream->pcm_storage || !stream->message || !stream->decoded)
    goto cleanup;
  stream->pcm = xStreamBufferCreateStatic(PCM_QUEUE_BYTES + 1, 1,
                                           stream->pcm_storage, &stream->pcm_control);
  if (!stream->pcm) goto cleanup;
  if (reminder) {
    // 短提醒等到session.finished再起播；长提醒接近队列容量时开始消费，
    // 留一个最大解码块的空间，不能等到完成事件却把网络回调堵在满队列上。
    size_t capacity = xStreamBufferSpacesAvailable(stream->pcm);
    prebuffer_bytes = capacity > WS_MESSAGE_LIMIT
        ? capacity - WS_MESSAGE_LIMIT : capacity / 2;
  }
  stage = "input_stack";
  atomic_store(&stream->input_done, false);
  // Same sender, same 8 KiB internal stack, allocated before either WSS task
  // creation or handshake. PCM consumption remains independent of sending.
  if (xTaskCreate(tts_input_task, "tts_input", 8192, stream, TTS_PLAY_PRIORITY, NULL) != pdPASS) {
    atomic_store(&stream->input_done, true);
    goto cleanup;
  }
  char headers[320];
  snprintf(headers, sizeof(headers), "Authorization: Bearer %s\r\n", CLOUD_DASHSCOPE_API_KEY);
  const esp_websocket_client_config_t config = {
      .uri = CLOUD_TTS_URL,
      .headers = headers,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .disable_auto_reconnect = true,
      .task_stack = 8192,
      .task_prio = TTS_PLAY_PRIORITY,
      .buffer_size = 2048,
      .network_timeout_ms = app_turn_remaining_ms(turn, esp_timer_get_time(), 10000),
  };
  stage = "websocket_init";
  client = esp_websocket_client_init(&config);
  if (!client) goto cleanup;
  stream->client = client;
  result = esp_websocket_register_events(client, WEBSOCKET_EVENT_ANY, websocket_event, stream);
  if (result != ESP_OK) goto cleanup;
  stage = "websocket_start";
  result = esp_websocket_client_start(client);
  if (result != ESP_OK) goto cleanup;
  started = true;
  app_turn_resources(turn, "TTS连接/队列已分配");
  stage = "session_created";
  if ((result = wait_event(stream, CREATED_BIT)) != ESP_OK) goto cleanup;
  stage = "session_update";
  if ((result = send_event(stream, "session.update", 1, NULL)) != ESP_OK ||
      (result = wait_event(stream, UPDATED_BIT)) != ESP_OK) goto cleanup;
  atomic_store(&stream->input_ready, true);
  stage = "streaming_playback";

  bool playing = false;
  bool buffering = true;
  bool pending = false;
  uint8_t pending_byte = 0;
  size_t played = 0;
  int64_t last_audio_us = esp_timer_get_time();
  // 2字节对齐；允许网络边界把一个16位采样拆开。
  int16_t block[512];
  while ((result = atomic_load(&stream->error)) == ESP_OK) {
    if (stopped(stream)) { result = ESP_ERR_INVALID_STATE; break; }
    report_first_pcm(stream, turn);
    bool finished = atomic_load(&stream->finished);
    size_t available = xStreamBufferBytesAvailable(stream->pcm);
    if (last_write_end && available < min_pcm_available) min_pcm_available = available;
    if (finished && !available) {
      if (pending || !played || !atomic_load(&stream->input_finished)) result = ESP_ERR_INVALID_SIZE;
      break;
    }
    int64_t now = esp_timer_get_time();
    if (app_turn_expired(turn, now) || now - stream->start_us > TTS_SESSION_LIMIT_US || now - last_audio_us > 30000000) {
      result = ESP_ERR_TIMEOUT;
      break;
    }
    if (playing && !available && !finished && !buffering) {
      buffering = true;
      starving_since = now;
      ++turn->tts_starvations;
      if (CONFIG_VOX_LOG_VERBOSE || turn->tts_starvations == 1 ||
          turn->tts_starvations % 5 == 0)
        ESP_LOGW(TAG, "[%s] PCM underrun: count=%u; buffering, not acoustic interruption",
                 app_log_id(turn->id), turn->tts_starvations);
    }
    if (buffering) {
      if (available < prebuffer_bytes && !finished) {
        vTaskDelay(pdMS_TO_TICKS(10));
        continue;
      }
      buffering = false;
      if (starving_since) {
        turn->tts_wait_us += now - starving_since;
        if (CONFIG_VOX_LOG_VERBOSE || turn->tts_starvations == 1 ||
            turn->tts_starvations % 5 == 0)
          ESP_LOGI(TAG, "[%s] PCM resumed: buffered=%u B, wait=%ums",
                   app_log_id(turn->id), (unsigned)available,
                   (unsigned)((now - starving_since) / 1000));
        starving_since = 0;
      }
      if (!playing) {
        app_audio_playback_diagnostics(true);
        // Keep the DMA producer ahead of microphone/AEC and WSS work (priority 5).
        // Blocking I2S/queue waits still let those tasks run; no extra DMA latency.
        if (output_priority < TTS_OUTPUT_PRIORITY) {
          vTaskPrioritySet(NULL, TTS_OUTPUT_PRIORITY);
          output_boosted = true;
        }
        playing = true;
        bool can_interrupt = !reminder && app_acoustic_listen(true);
        if (!reminder && !can_interrupt) app_barge_diagnostic_end(0, BD_UNAVAILABLE);
        if (reminder)
          APP_LOG_DETAIL(TAG, "[%s] 提醒起播：完整接收=%d，缓冲=%u B，优先级=%u",
                   app_log_id(turn->id), finished, (unsigned)available,
                   (unsigned)uxTaskPriorityGet(NULL));
        app_ui_show_voice_state(reminder ? APP_UI_VOICE_REMINDER : APP_UI_VOICE_SPEAKING,
                               reminder ? text : can_interrupt ? "小冰小冰可打断" : "按键可停止回答");
        app_acoustic_playing(true);
      }
    }
    size_t prefix = pending ? 1 : 0;
    uint8_t *bytes = (uint8_t *)block;
    if (pending) bytes[0] = pending_byte;
    size_t n = xStreamBufferReceive(stream->pcm, bytes + prefix,
                                     sizeof(block) - prefix, pdMS_TO_TICKS(100));
    if (!n) continue;
    last_audio_us = esp_timer_get_time();
    n += prefix;
    pending = (n & 1) != 0;
    if (pending) pending_byte = bytes[n - 1];
    n &= ~(size_t)1;
    if (n) {
      for (size_t i = 0; i < n / 2; ++i) {
        uint32_t amplitude = block[i] < 0 ? (uint32_t)(-(int32_t)block[i]) : (uint32_t)block[i];
        if (amplitude > peak) peak = amplitude;
        absolute_sum += amplitude;
      }
      sample_total += n / 2;
      if (!played && turn->times[TURN_PLAY_START] < 0) {
        // 资源日志在首块之前输出，避免阻塞连续PCM写入造成音频间隙。
        app_turn_resources(turn, "播放准备");
        app_turn_emit(turn, TURN_PLAY_START, NULL, ESP_OK);
      }
      int64_t write_start = esp_timer_get_time();
      if (last_write_end && write_start - last_write_end > max_feed_gap_us)
        max_feed_gap_us = write_start - last_write_end;
      result = app_audio_play_mono_pcm(block, n / 2);
      last_write_end = esp_timer_get_time();
      if (last_write_end - write_start > max_write_us)
        max_write_us = last_write_end - write_start;
      if (result != ESP_OK) break;
      played += n;
      turn->pcm_submitted_bytes += n;
    }
  }
cleanup:
  if (output_boosted) vTaskPrioritySet(NULL, output_priority);
  if (starving_since) turn->tts_wait_us += esp_timer_get_time() - starving_since;
  if (stopped(stream)) result = ESP_ERR_INVALID_STATE;
  if (!app_cloud_is_cancelled() && app_turn_expired(turn, esp_timer_get_time())) result = ESP_ERR_TIMEOUT;
  bool audio_aborted = false;
  if (result != ESP_OK) {
    if (stopped(stream))
      APP_LOG_DETAIL(TAG, "[%s] TTS cancelled: stage=%s", app_log_id(turn->id), stage);
    else
      ESP_LOGE(TAG, "[%s] TTS failed: stage=%s, error=%s, internal=%u B, largest=%u B, PSRAM=%u B",
               app_log_id(turn->id), stage, esp_err_to_name(result),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
               (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    // 在等待网络回调退出之前停止硬件；仅停止提交PCM不等于停止DMA。
    atomic_store(&stream->error, result);
    esp_err_t stopped = app_audio_abort_playback();
    if (stopped != ESP_OK) ESP_LOGE(TAG, "停止音频输出失败：%s", esp_err_to_name(stopped));
    app_acoustic_playing(false);
    audio_aborted = true;
  }
  // Snapshot output before network cleanup adds idle DMA cycles to the stats.
  if (last_write_end) app_audio_playback_diagnostics(false);
  if (last_write_end)
    ESP_LOGI(TAG, "[%s] playback_diag: feed_gap_max=%ums write_max=%ums queue_min=%uB",
             app_log_id(turn->id), (unsigned)(max_feed_gap_us / 1000),
             (unsigned)(max_write_us / 1000),
             (unsigned)(min_pcm_available == SIZE_MAX ? 0 : min_pcm_available));
  // 不能在网络发送任务仍访问client/stream/feed_context时销毁它们。
  while (!atomic_load(&stream->input_done)) vTaskDelay(pdMS_TO_TICKS(10));
  if (result == ESP_OK) result = atomic_load(&stream->error);
  if (stopped(stream)) result = ESP_ERR_INVALID_STATE;
  if (result != ESP_OK && !audio_aborted) (void)app_audio_abort_playback();
  turn->tts_text_chunks += atomic_load(&stream->text_chunks);
  APP_LOG_DETAIL(TAG, "[%s] TTS PCM幅度(音量缩放前)：采样=%u，峰值=%u/32768，平均绝对幅度=%u，结果=%s",
           app_log_id(turn->id), (unsigned)sample_total, (unsigned)peak,
           (unsigned)(sample_total ? absolute_sum / sample_total : 0), esp_err_to_name(result));
  // 先终止并等待回调退出，之后才释放回调使用的队列和内存。
  if (client) {
    if (started) esp_websocket_client_stop(client);
    esp_websocket_client_destroy(client);
  }
  report_first_pcm(stream, turn);
  APP_LOG_DETAIL(TAG, "[%s] TTS结果=%s", app_log_id(turn->id), esp_err_to_name(result));
  if (stream->pcm) vStreamBufferDelete(stream->pcm);
  if (stream->events) vEventGroupDelete(stream->events);
  free(stream->pcm_storage); free(stream->message); free(stream->decoded); free(stream);
  app_acoustic_playing(false);
  app_acoustic_listen(false);
  return result;
}

esp_err_t app_cloud_speak(const char *text, app_turn_t *turn) {
  return speak(text, NULL, NULL, turn, false, NULL);
}

esp_err_t app_cloud_speak_stream(app_tts_feed_t feed, void *context,
                                 app_turn_t *turn, const _Atomic bool *abort) {
  if (!feed) return ESP_ERR_INVALID_ARG;
  return speak(NULL, feed, context, turn, false, abort);
}

esp_err_t app_cloud_speak_reminder(const char *text, app_turn_t *turn) {
  // 到期扫描/索引任务原为优先级3，低于LVGL(4)。仅播报期间与普通
  // TTS播放任务保持相同优先级，退出后恢复，不提升后台文件操作的优先级。
  UBaseType_t priority = uxTaskPriorityGet(NULL);
  if (priority < TTS_PLAY_PRIORITY) vTaskPrioritySet(NULL, TTS_PLAY_PRIORITY);
  esp_err_t result = speak(text, NULL, NULL, turn, true, NULL);
  if (priority < TTS_PLAY_PRIORITY) vTaskPrioritySet(NULL, priority);
  return result;
}
