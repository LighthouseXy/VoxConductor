/*
 * 纯轮次状态机：记录单调时间、阶段成功/失败/取消与模型usage。
 * 同步消费事件，终态后拒绝后续事件，避免重入伪造成功。
 */
#include "app_turn.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

void app_turn_init(app_turn_t *t, const char *boot, uint32_t sequence, int64_t now) {
  memset(t, 0, sizeof(*t));
  snprintf(t->session_id, sizeof(t->session_id), "%.32s", boot);
  snprintf(t->id, sizeof(t->id), "%s-%" PRIu32, t->session_id, sequence);
  t->state = TURN_RUNNING;
  t->started_us = t->accepted_us = now;
  t->deadline_us = now + APP_TURN_BUDGET_US;
  t->first_token_us = -1;
  for (int i = 0; i < TURN_EVENT_COUNT; ++i) t->times[i] = -1;
  t->prompt_tokens = t->completion_tokens = t->total_tokens = -1;
}

const char *app_turn_event_name(app_turn_event_type_t e) {
  static const char *names[] = {
    "录音开始", "录音结束", "识别开始", "用户文本", "生成开始", "助手文本",
    "合成开始", "首PCM到达", "首次送I2S", "PCM提交结束", "TTS调用结束",
    "轮次完成", "轮次失败", "轮次取消"
  };
  return (unsigned)e < TURN_EVENT_COUNT ? names[e] : "未知事件";
}

bool app_turn_apply(app_turn_t *t, app_turn_event_type_t e, int64_t now,
                    const char *text, int error, app_turn_event_t *out) {
  if (!t || !out || (unsigned)e >= TURN_EVENT_COUNT || t->state != TURN_RUNNING ||
      now < t->started_us || t->times[e] >= 0) return false;
  if ((e == TURN_USER_TEXT || e == TURN_ASSISTANT_TEXT) && (!text || !*text)) return false;
  int stage = -1;
  bool start = false;
  switch (e) {
    case TURN_RECORD_START: stage = TURN_RECORD; start = true; break;
    case TURN_RECORD_END: stage = TURN_RECORD; break;
    case TURN_ASR_START: stage = TURN_ASR; start = true; break;
    case TURN_USER_TEXT: stage = TURN_ASR; break;
    case TURN_GENERATE_START: stage = TURN_GENERATE; start = true; break;
    case TURN_ASSISTANT_TEXT: stage = TURN_GENERATE; break;
    case TURN_TTS_START: stage = TURN_SYNTHESIZE; start = true; break;
    case TURN_TTS_END: stage = TURN_SYNTHESIZE; break;
    case TURN_PLAY_START: stage = TURN_PLAY; start = true; break;
    case TURN_PLAY_END: stage = TURN_PLAY; break;
    case TURN_FIRST_PCM:
      if (t->stages[TURN_SYNTHESIZE] != TURN_RUNNING) return false;
      break;
    case TURN_DONE:
      for (int i = 0; i < TURN_STAGE_COUNT; ++i)
        if (t->stages[i] != TURN_SUCCEEDED) return false;
      t->state = TURN_SUCCEEDED;
      break;
    case TURN_ERROR:
    case TURN_CANCEL:
      t->state = e == TURN_ERROR ? TURN_FAILED : TURN_CANCELLED;
      t->error = error;
      for (int i = 0; i < TURN_STAGE_COUNT; ++i)
        if (t->stages[i] == TURN_RUNNING) t->stages[i] = t->state;
      break;
    default: return false;
  }
  if (stage >= 0) {
    if (t->stages[stage] != (start ? TURN_PENDING : TURN_RUNNING)) return false;
    static const app_turn_event_type_t starts[] = {
      TURN_RECORD_START, TURN_ASR_START, TURN_GENERATE_START, TURN_TTS_START, TURN_PLAY_START
    };
    if (!start && now < t->times[starts[stage]]) return false;
    t->stages[stage] = start ? TURN_RUNNING : TURN_SUCCEEDED;
  }
  t->times[e] = now;
  *out = (app_turn_event_t){t->id, ++t->event_sequence, e, now, text, error};
  return true;
}

static int64_t token_count(const cJSON *usage, const char *key) {
  const cJSON *n = cJSON_GetObjectItemCaseSensitive(usage, key);
  // 避免非整数、负值、溢出或异常大字段进入统计。
  if (!cJSON_IsNumber(n) || !isfinite(n->valuedouble) || n->valuedouble < 0 ||
      n->valuedouble > 2147483647 || floor(n->valuedouble) != n->valuedouble) return -1;
  return (int64_t)n->valuedouble;
}

void app_turn_read_usage(app_turn_t *t, const cJSON *root) {
  const cJSON *usage = cJSON_GetObjectItemCaseSensitive(root, "usage");
  t->prompt_tokens = token_count(usage, "prompt_tokens");
  t->completion_tokens = token_count(usage, "completion_tokens");
  t->total_tokens = token_count(usage, "total_tokens");
}
