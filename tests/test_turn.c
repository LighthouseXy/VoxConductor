#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_turn.h"

static void event(app_turn_t *t, app_turn_event_type_t e, int64_t time, const char *text) {
  app_turn_event_t out;
  assert(app_turn_apply(t, e, time, text, 0, &out));
  assert(out.sequence == t->event_sequence);
  assert(strcmp(out.turn_id, t->id) == 0);
  assert(out.type == e && out.at_us == time && out.text == text);
}

int main(void) {
  app_turn_t t;
  app_turn_event_t out;
  app_turn_init(&t, "boot-a", 1, 0);
  assert(!strcmp(t.id, "boot-a-1"));
  assert(t.times[TURN_FIRST_PCM] == -1 && t.prompt_tokens == -1);
  assert(!app_turn_apply(&t, TURN_DONE, 0, NULL, 0, &out));
  assert(!app_turn_apply(&t, TURN_RECORD_END, 0, NULL, 0, &out));
  event(&t, TURN_RECORD_START, 0, NULL); // monotonic zero is a valid timestamp
  assert(!app_turn_apply(&t, TURN_RECORD_START, 1, NULL, 0, &out));
  event(&t, TURN_RECORD_END, 10, NULL);
  event(&t, TURN_ASR_START, 11, NULL);
  assert(!app_turn_apply(&t, TURN_USER_TEXT, 10, "早于开始", 0, &out));
  assert(!app_turn_apply(&t, TURN_USER_TEXT, 12, "", 0, &out));
  event(&t, TURN_USER_TEXT, 20, "用户原文");
  event(&t, TURN_GENERATE_START, 21, NULL);
  event(&t, TURN_ASSISTANT_TEXT, 30, "助手原文");
  event(&t, TURN_TTS_START, 31, NULL);
  event(&t, TURN_FIRST_PCM, 40, NULL);
  event(&t, TURN_PLAY_START, 45, NULL);
  assert(t.stages[TURN_SYNTHESIZE] == TURN_RUNNING && t.stages[TURN_PLAY] == TURN_RUNNING);
  event(&t, TURN_PLAY_END, 80, NULL);
  event(&t, TURN_TTS_END, 85, NULL);
  event(&t, TURN_DONE, 90, NULL);
  assert(t.state == TURN_SUCCEEDED);
  assert(!app_turn_apply(&t, TURN_ERROR, 100, NULL, 1, &out));

  // 失败、取消均关闭运行阶段，之后的迟到事件不能更改终态。
  for (int terminal = TURN_ERROR; terminal <= TURN_CANCEL; ++terminal) {
    app_turn_init(&t, "boot-a", 2, 100);
    event(&t, TURN_GENERATE_START, 101, NULL);
    event(&t, TURN_TTS_START, 102, NULL);
    event(&t, TURN_PLAY_START, 103, NULL);
    assert(app_turn_apply(&t, terminal, 104, NULL, 123, &out));
    assert(t.error == 123);
    assert(t.stages[TURN_GENERATE] == t.state && t.stages[TURN_PLAY] == t.state);
    assert(t.stages[TURN_ASR] == TURN_PENDING);
    assert(!app_turn_apply(&t, TURN_ASSISTANT_TEXT, 105, "迟到", 0, &out));
  }
  app_turn_init(&t, "boot-b", 1, 0);
  assert(!strcmp(t.id, "boot-b-1")); // boot ID distinguishes reset without wall clock
  assert(app_turn_apply(&t, TURN_ERROR, 0, NULL, 1, &out)); // offline/allocation failure
  assert(t.times[TURN_RECORD_START] == -1);

  cJSON *root = cJSON_Parse("{\"usage\":{\"prompt_tokens\":120,\"completion_tokens\":30,\"total_tokens\":150}}");
  assert(root);
  app_turn_read_usage(&t, root);
  assert(t.prompt_tokens == 120 && t.completion_tokens == 30 && t.total_tokens == 150);
  cJSON_Delete(root);
  root = cJSON_Parse("{\"usage\":{\"prompt_tokens\":-1,\"completion_tokens\":1.5,\"total_tokens\":999999999999}}");
  app_turn_read_usage(&t, root);
  assert(t.prompt_tokens == -1 && t.completion_tokens == -1 && t.total_tokens == -1);
  cJSON_Delete(root);
  root = cJSON_Parse("{\"usage\":{\"prompt_tokens\":0,\"total_tokens\":\"42\"}}");
  app_turn_read_usage(&t, root);
  assert(t.prompt_tokens == 0 && t.completion_tokens == -1 && t.total_tokens == -1);
  cJSON_Delete(root);
  app_turn_read_usage(&t, NULL);
  assert(t.prompt_tokens == -1 && t.total_tokens == -1);
  puts("PASS: turn IDs/events/overlapping stages/failure/cancel/late events/usage validation");
}
