#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

// 普通串口只预览用户识别文本；详细模式可显示全文。设为0可全部隐藏正文。
#ifndef APP_TURN_LOG_TEXT
#define APP_TURN_LOG_TEXT 1
#endif

typedef enum {
  TURN_RECORD, TURN_ASR, TURN_GENERATE, TURN_SYNTHESIZE, TURN_PLAY, TURN_STAGE_COUNT
} app_turn_stage_t;
typedef enum {
  TURN_PENDING, TURN_RUNNING, TURN_SUCCEEDED, TURN_FAILED, TURN_CANCELLED
} app_turn_state_t;
typedef enum {
  TURN_RECORD_START, TURN_RECORD_END, TURN_ASR_START, TURN_USER_TEXT,
  TURN_GENERATE_START, TURN_ASSISTANT_TEXT, TURN_TTS_START, TURN_FIRST_PCM,
  TURN_PLAY_START, TURN_PLAY_END, TURN_TTS_END,
  TURN_DONE, TURN_ERROR, TURN_CANCEL, TURN_EVENT_COUNT
} app_turn_event_type_t;

// M1可使用同一字段持久化。text只在同步处理事件期间有效，异步写盘必须复制。
typedef struct {
  const char *turn_id;
  uint32_t sequence;
  app_turn_event_type_t type;
  int64_t at_us;
  const char *text;
  int error;
} app_turn_event_t;

typedef struct {
  char id[64];
  char session_id[33];  // 当前会话=本次启动；M1再增加跨重启恢复。
  uint32_t event_sequence;
  app_turn_state_t state;
  app_turn_state_t stages[TURN_STAGE_COUNT];
  int64_t started_us;
  int64_t accepted_us, deadline_us;
  int64_t first_token_us;
  size_t played_text_bytes; // 连续TTS仅全文成功后确认；中断时文字/PCM无精确映射，记为0。
  int64_t times[TURN_EVENT_COUNT]; // -1=未发生；非流式没有首token时间。
  int error;
  size_t history_turns, history_bytes, asr_request_bytes, llm_request_bytes;
  size_t pcm_submitted_bytes; // I2S调用成功提交的字节；实际听到的内容需声学确认。
  unsigned tts_batches, agent_requests, agent_tools;
  unsigned tts_text_chunks, tts_starvations;
  int64_t tts_wait_us; // 起播后因PCM耗尽等待的累计时间，不含首次预缓冲。
  int64_t llm_done_us; // 最终文本接收结束（不含后续等待播完），0=尚未完成。
  int64_t summary_wait_us; // 前台等后台TLS自行清理，不含录音或ASR连接。
  int64_t prompt_tokens, completion_tokens, total_tokens; // -1=未知。
} app_turn_t;

void app_turn_init(app_turn_t *turn, const char *boot_id, uint32_t sequence, int64_t now);
bool app_turn_apply(app_turn_t *turn, app_turn_event_type_t type, int64_t now,
                    const char *text, int error, app_turn_event_t *event);
void app_turn_read_usage(app_turn_t *turn, const cJSON *root);
const char *app_turn_event_name(app_turn_event_type_t type);

// 以下由ESP适配实现。轮次数据由单任务独占；播放任务用独立副本，join后合并。
// 不从网络回调直接修改。
void app_turn_begin(app_turn_t *turn);
void app_turn_emit(app_turn_t *turn, app_turn_event_type_t type, const char *text, int error);
void app_turn_emit_at(app_turn_t *turn, app_turn_event_type_t type, int64_t at_us);
void app_turn_resources(const app_turn_t *turn, const char *point);
void app_turn_report(const app_turn_t *turn);

// Initial total budget reuses the former Agent limit; no per-stage renewal.
#define APP_TURN_BUDGET_US 180000000LL
static inline bool app_turn_expired(const app_turn_t *turn, int64_t now) {
  return turn && turn->deadline_us > 0 && now >= turn->deadline_us;
}
static inline unsigned app_turn_remaining_ms(const app_turn_t *turn, int64_t now, unsigned cap) {
  if (!turn || turn->deadline_us <= 0) return cap;
  if (now >= turn->deadline_us) return 0;
  int64_t remaining = (turn->deadline_us - now + 999) / 1000;
  return remaining < (int64_t)cap ? (unsigned)remaining : cap;
}
