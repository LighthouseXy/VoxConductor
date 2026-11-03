/*
 * 轮次ESP适配：先记录历史，再输出简洁日志和按需资源诊断。
 * 串口缩短ID/正文只影响显示，不修改存储的来源或原文。
 * 阶段时间记录软件事件，声学首声需独立测量。
 */
#include "app_turn.h"
#include "app_storage.h"
#include "app_log.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "voice";

void app_turn_resources(const app_turn_t *t, const char *point) {
  unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
  APP_LOG_DETAIL(TAG, "[%s] 资源/%s：内部空闲=%u，内部历史最低=%u，最大连续块=%u，PSRAM空闲=%u，PSRAM历史最低=%u，当前任务栈余量=%u B",
           app_log_id(t->id), point, (unsigned)heap_caps_get_free_size(caps),
           (unsigned)heap_caps_get_minimum_free_size(caps),
           (unsigned)heap_caps_get_largest_free_block(caps),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
}

void app_turn_begin(app_turn_t *t) {
  // 单语音任务入口串行；启动随机标识不依赖SNTP，也不暴露MAC地址。
  static char boot[33];
  static uint32_t sequence;
  if (!boot[0] || sequence == UINT32_MAX) {
    uint32_t r[4];
    esp_fill_random(r, sizeof(r));
    snprintf(boot, sizeof(boot), "%08" PRIx32 "%08" PRIx32 "%08" PRIx32 "%08" PRIx32,
             r[0], r[1], r[2], r[3]);
    sequence = 0;
  }
  app_turn_init(t, boot, ++sequence, esp_timer_get_time());
  ESP_LOGI(TAG, "[%s] 开始", app_log_id(t->id));
  app_turn_resources(t, "开始");
}

#if APP_TURN_LOG_TEXT
static void log_text(const app_turn_t *t, app_turn_event_type_t type, const char *text) {
  const char *value = text ? text : "";
#if !CONFIG_VOX_LOG_VERBOSE
  if (type == TURN_ASSISTANT_TEXT) {
    ESP_LOGI(TAG, "[%s] 回答文本已生成: %u B", app_log_id(t->id), (unsigned)strlen(value));
    return;
  }
  char preview[97];
  size_t n = strlen(value);
  bool truncated = n >= sizeof(preview);
  if (truncated) {
    n = sizeof(preview) - 1;
    // A valid UTF-8 code point must never be cut in the middle.
    while (n && ((unsigned char)value[n] & 0xc0) == 0x80) --n;
  }
  memcpy(preview, value, n);
  preview[n] = 0;
  value = preview;
#else
  bool truncated = false;
#endif
  // Escape newlines and control characters, even in a short preview.
  cJSON *json = cJSON_CreateString(value);
  char *safe = json ? cJSON_PrintUnformatted(json) : NULL;
  ESP_LOGI(TAG, "[%s] %s: %s%s", app_log_id(t->id),
           type == TURN_USER_TEXT ? "识别" : "助手", safe ? safe : "(文本显示失败)",
           truncated ? "..." : "");
  cJSON_free(safe);
  cJSON_Delete(json);
}
#endif

static void emit(app_turn_t *t, app_turn_event_type_t type, int64_t at,
                 const char *text, int error) {
  app_turn_event_t event;
  if (!app_turn_apply(t, type, at, text, error, &event)) {
    ESP_LOGW(TAG, "[%s] 忽略无效事件：%s", app_log_id(t->id), app_turn_event_name(type));
    return;
  }
  app_storage_record_event(t, &event);
  if (type == TURN_USER_TEXT || type == TURN_ASSISTANT_TEXT) {
#if APP_TURN_LOG_TEXT
    log_text(t, type, text);
#else
    ESP_LOGI(TAG, "[%s] %s（正文已隐藏）", app_log_id(t->id), app_turn_event_name(type));
#endif
  } else if (type == TURN_RECORD_START || type == TURN_GENERATE_START ||
             type == TURN_PLAY_START) {
    ESP_LOGI(TAG, "[%s] %s", app_log_id(t->id),
             type == TURN_RECORD_START ? "聆听" :
             type == TURN_GENERATE_START ? "生成回答" : "开始播放");
  } else {
    APP_LOG_DETAIL(TAG, "[%s] 事件=%s 序号=%" PRIu32 " +%.3fs 结果=%s", app_log_id(t->id),
             app_turn_event_name(type), event.sequence, (at - t->started_us) / 1e6,
             esp_err_to_name(error));
  }
}

void app_turn_emit(app_turn_t *t, app_turn_event_type_t e, const char *text, int error) {
  emit(t, e, esp_timer_get_time(), text, error);
}

void app_turn_emit_at(app_turn_t *t, app_turn_event_type_t e, int64_t at) {
  emit(t, e, at, NULL, ESP_OK);
}

static void duration(char *out, size_t size, int64_t start, int64_t end) {
  if (start < 0 || end < start) snprintf(out, size, "未完成/未知");
  else snprintf(out, size, "%.2fs", (end - start) / 1e6);
}

void app_turn_report(const app_turn_t *t) {
  char tail[32], playback[32], total[32], accepted[32];
  duration(tail, sizeof(tail), t->times[TURN_RECORD_END], t->times[TURN_USER_TEXT]);
  duration(playback, sizeof(playback), t->times[TURN_RECORD_END], t->times[TURN_PLAY_START]);
  int64_t ended = t->times[TURN_DONE];
  if (ended < 0) ended = t->times[TURN_ERROR];
  if (ended < 0) ended = t->times[TURN_CANCEL];
  duration(total, sizeof(total), t->times[TURN_RECORD_END], ended);
  duration(accepted, sizeof(accepted), t->accepted_us, ended);
  ESP_LOGI(TAG, "[%s] 准入后软件耗时=%s，预算=%s", app_log_id(t->id), accepted,
           app_turn_expired(t, ended) ? "已到期" : "未到期");
  ESP_LOGI(TAG, "[%s] 结束: %s/%s | 录音后识别=%s 首I2S=%s 录音后总计=%s",
           app_log_id(t->id), t->state == TURN_SUCCEEDED ? "成功" :
           t->state == TURN_CANCELLED ? "取消" : "失败", esp_err_to_name(t->error),
           tail, playback, total);
  ESP_LOGI(TAG, "[%s] 统计: 模型=%u 工具=%u 补缓冲=%u次/%.2fs PCM=%u B",
           app_log_id(t->id), t->agent_requests, t->agent_tools,
           t->tts_starvations, t->tts_wait_us / 1e6, (unsigned)t->pcm_submitted_bytes);
  // 日常日志增加一行关键阶段。声学首声需另测；ASR连接与录音可
  // 重叠，端到端延迟按实际起止时刻测量。
  char generation[32], startup[32];
  duration(generation, sizeof(generation), t->times[TURN_GENERATE_START], t->times[TURN_ASSISTANT_TEXT]);
  duration(startup, sizeof(startup), t->times[TURN_TTS_START], t->times[TURN_PLAY_START]);
  ESP_LOGI(TAG, "[%s] 延迟分段: 识别尾等待=%s 模型=%s TTS起播=%s 后台让出=%.2fs",
           app_log_id(t->id), tail, generation, startup, t->summary_wait_us / 1e6);
  if (t->state == TURN_FAILED) {
    unsigned caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGE(TAG, "[%s] 失败内存: 内部=%u 最低=%u 最大块=%u PSRAM=%u 栈余量=%u B",
             app_log_id(t->id), (unsigned)heap_caps_get_free_size(caps),
             (unsigned)heap_caps_get_minimum_free_size(caps),
             (unsigned)heap_caps_get_largest_free_block(caps),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)uxTaskGetStackHighWaterMark(NULL));
  }
#if CONFIG_VOX_LOG_VERBOSE
  char asr[32], llm[32], tts[32], first[32];
  duration(asr, sizeof(asr), t->times[TURN_ASR_START], t->times[TURN_USER_TEXT]);
  duration(llm, sizeof(llm), t->times[TURN_GENERATE_START], t->times[TURN_ASSISTANT_TEXT]);
  duration(tts, sizeof(tts), t->times[TURN_TTS_START], t->times[TURN_TTS_END]);
  duration(first, sizeof(first), t->times[TURN_TTS_START], t->times[TURN_FIRST_PCM]);
  ESP_LOGI(TAG, "[%s] 结果=%s/%s | 历史=%u轮/%u B | 请求 ASR=%u B，LLM=%u B",
           app_log_id(t->id), t->state == TURN_SUCCEEDED ? "成功" : t->state == TURN_CANCELLED ? "取消" : "失败",
           esp_err_to_name(t->error), (unsigned)t->history_turns, (unsigned)t->history_bytes,
           (unsigned)t->asr_request_bytes, (unsigned)t->llm_request_bytes);
  ESP_LOGI(TAG, "[%s] 耗时：识别全程(含录音)=%s，回答流水线=%s，TTS跨度=%s，录音结束至轮次结束=%s",
           app_log_id(t->id), asr, llm, tts, total);
  ESP_LOGI(TAG, "[%s] 松开后识别等待=%s", app_log_id(t->id), tail);
  char token[32];
  duration(token, sizeof(token), t->times[TURN_GENERATE_START], t->first_token_us);
  ESP_LOGI(TAG, "[%s] TTS首PCM=%s | 录音结束至首次送I2S=%s（非声学首声）| PCM已提交=%u B | 首文本=%s",
           app_log_id(t->id), first, playback, (unsigned)t->pcm_submitted_bytes, token);
  char in[24], out[24], sum[24];
  if (t->prompt_tokens < 0) snprintf(in, sizeof(in), "未知");
  else snprintf(in, sizeof(in), "%" PRId64, t->prompt_tokens);
  if (t->completion_tokens < 0) snprintf(out, sizeof(out), "未知");
  else snprintf(out, sizeof(out), "%" PRId64, t->completion_tokens);
  if (t->total_tokens < 0) snprintf(sum, sizeof(sum), "未知");
  else snprintf(sum, sizeof(sum), "%" PRId64, t->total_tokens);
  ESP_LOGI(TAG, "[%s] LLM tokens：输入=%s，输出=%s，合计=%s", app_log_id(t->id), in, out, sum);
  char received[32];
  duration(received, sizeof(received), t->times[TURN_GENERATE_START], t->llm_done_us > 0 ? t->llm_done_us : -1);
  ESP_LOGI(TAG, "[%s] Agent模型请求=%u，工具调用=%u，TTS会话=%u，文本片段=%u，音频补缓冲=%u次/%.2fs，最终文本接收完成=%s",
           app_log_id(t->id), t->agent_requests, t->agent_tools, t->tts_batches, t->tts_text_chunks,
           t->tts_starvations, t->tts_wait_us / 1e6, received);
  app_turn_resources(t, "结束");
#endif
}
