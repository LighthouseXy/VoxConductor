#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "app_speech.h"
#include "app_cloud.h"
#include "app_conversation.h"
#include "freertos/task.h"

static _Atomic bool cancelled, speaking, hold, fail_tts;
static bool fail_task;
static pthread_t worker;
static char played[APP_REPLY_TEXT_BYTES];
static unsigned calls;
typedef struct { void (*fn)(void *); void *arg; } launch_t;
static void *launch(void *p) {
  launch_t copy = *(launch_t *)p; free(p); copy.fn(copy.arg); return NULL;
}
int xTaskCreate(void (*fn)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle) {
  (void)name; (void)priority; (void)handle; assert(stack >= 8192);
  if (fail_task) return 0;
  launch_t *p = malloc(sizeof(*p)); assert(p); *p = (launch_t){fn, arg};
  assert(!pthread_create(&worker, NULL, launch, p)); return pdPASS;
}
void vTaskDelete(void *task) { assert(!task); pthread_exit(NULL); }
void vTaskDelay(TickType_t ticks) {
  struct timespec t = {.tv_sec = ticks / 1000, .tv_nsec = (ticks % 1000) * 1000000L};
  nanosleep(&t, NULL);
}
int64_t esp_timer_get_time(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
bool app_cloud_is_cancelled(void) { return atomic_load(&cancelled); }
void app_turn_emit(app_turn_t *t, app_turn_event_type_t type, const char *text, int error) {
  app_turn_event_t event;
  assert(app_turn_apply(t, type, esp_timer_get_time(), text, error, &event));
}
esp_err_t app_cloud_speak_stream(app_tts_feed_t feed, void *context, app_turn_t *t,
                                const _Atomic bool *abort) {
  ++calls;
  ++t->tts_batches;
  atomic_store(&speaking, true);
  while (atomic_load(&hold) && !atomic_load(abort) && !app_cloud_is_cancelled()) vTaskDelay(1);
  if (atomic_load(abort) || app_cloud_is_cancelled()) return ESP_ERR_INVALID_STATE;
  if (atomic_load(&fail_tts)) return ESP_FAIL;
  bool final = false;
  while (!final) {
    if (atomic_load(abort) || app_cloud_is_cancelled()) return ESP_ERR_INVALID_STATE;
    char text[481];
    esp_err_t r = feed(context, text, &final);
    if (r != ESP_OK) return r;
    if (*text) {
      if (t->times[TURN_PLAY_START] < 0) app_turn_emit(t, TURN_PLAY_START, NULL, ESP_OK);
      assert(strlen(played) + strlen(text) < sizeof(played)); strcat(played, text);
      t->pcm_submitted_bytes += strlen(text) * 2;
      ++t->tts_text_chunks;
    }
    vTaskDelay(1);
  }
  return ESP_OK;
}
static app_turn_t fresh(void) {
  app_turn_t t; app_turn_init(&t, "test", 1, esp_timer_get_time());
  atomic_store(&cancelled, false); atomic_store(&speaking, false);
  atomic_store(&hold, false); atomic_store(&fail_tts, false);
  played[0] = 0; calls = 0; return t;
}
static void wait_speaking(void) {
  for (int i = 0; i < 2000 && !atomic_load(&speaking); ++i) vTaskDelay(1);
  assert(atomic_load(&speaking));
}
static esp_err_t finish(app_speech_t *s, app_turn_t *t, bool ok) {
  esp_err_t r = app_speech_finish(s, t, ok); assert(!pthread_join(worker, NULL)); return r;
}
int main(void) {
  const char *first = "这是第一句完整的回答。";
  const char *tail = "第二句。第三句。";
  assert(!app_speech_chunk("你好。", 9, false, true, 0));
  assert(app_speech_chunk("你好。", 9, false, true, 250) == 9);
  assert(app_speech_chunk(tail, strlen(tail), true, false, 0) == strlen(tail));
  char utf8[600]; memset(utf8, 'a', 179); strcpy(utf8 + 179, "你好");
  assert(app_speech_chunk(utf8, strlen(utf8), false, true, 0) == 179);
  char exact[180]; memset(exact, 'a', sizeof(exact)); // 无结尾NUL，禁止读published以外
  assert(app_speech_chunk(exact, sizeof(exact), false, true, 0) == sizeof(exact));
  app_turn_t t = fresh(); atomic_store(&hold, true);
  app_speech_t *s = app_speech_start(&t); assert(s);
  assert(app_speech_append(s, first, strlen(first)) == ESP_OK); wait_speaking();
  // 播放阻塞时，生产者仍能追加后续文本，且turn元数据仍由生产者独占。
  assert(app_speech_append(s, tail, strlen(tail)) == ESP_OK);
  t.prompt_tokens = 123; t.first_token_us = 456; t.agent_requests = 2;
  atomic_store(&hold, false); assert(finish(s, &t, true) == ESP_OK);
  char expected[256]; snprintf(expected, sizeof(expected), "%s%s", first, tail);
  assert(!strcmp(played, expected)); assert(calls == 1 && t.tts_batches == 1);
  assert(t.played_text_bytes == strlen(expected) && t.pcm_submitted_bytes == strlen(expected) * 2);
  assert(t.prompt_tokens == 123 && t.first_token_us == 456 && t.agent_requests == 2);
  assert(t.event_sequence == 2 && t.times[TURN_TTS_START] >= 0 && t.times[TURN_PLAY_START] >= 0);
  // 全文已收完时合并短句，不为每个句号新建TTS。
  t = fresh(); s = app_speech_start(&t); assert(s);
  assert(app_speech_append(s, tail, strlen(tail)) == ESP_OK);
  assert(finish(s, &t, true) == ESP_OK && calls == 1 && !strcmp(played, tail));
  // 取消正在播的首句，同时丢弃已排队尾句；不能标记成完整播放。
  t = fresh(); atomic_store(&hold, true); s = app_speech_start(&t); assert(s);
  assert(app_speech_append(s, first, strlen(first)) == ESP_OK); wait_speaking();
  assert(app_speech_append(s, tail, strlen(tail)) == ESP_OK);
  atomic_store(&cancelled, true);
  assert(finish(s, &t, true) == ESP_ERR_INVALID_STATE);
  assert(calls == 1 && !played[0] && !t.played_text_bytes);
  // SSE失败只中止本条流水线，不改写全局用户取消标记。
  t = fresh(); atomic_store(&hold, true); s = app_speech_start(&t); assert(s);
  assert(app_speech_append(s, first, strlen(first)) == ESP_OK); wait_speaking();
  assert(finish(s, &t, false) == ESP_ERR_INVALID_STATE && !app_cloud_is_cancelled());
  // 下一轮不携带旧队列或旧取消状态。
  t = fresh(); s = app_speech_start(&t); assert(s);
  assert(app_speech_append(s, "新回答", strlen("新回答")) == ESP_OK);
  assert(finish(s, &t, true) == ESP_OK && !strcmp(played, "新回答"));
  t = fresh(); atomic_store(&fail_tts, true); s = app_speech_start(&t); assert(s);
  assert(app_speech_append(s, first, strlen(first)) == ESP_OK);
  assert(finish(s, &t, true) == ESP_FAIL && !t.played_text_bytes);
  // 多片长文本仍只建立一次会话，全文顺序不丢字。
  t = fresh(); s = app_speech_start(&t); assert(s);
  char long_text[10000]; memset(long_text, 'x', sizeof(long_text) - 1); long_text[9999] = 0;
  assert(app_speech_append(s, long_text, strlen(long_text)) == ESP_OK);
  assert(finish(s, &t, true) == ESP_OK && calls == 1 && t.tts_text_chunks >= 4);
  assert(!strcmp(played, long_text));
  // 多次跨环形物理边界仍保持UTF-8完整、全文顺序与单会话。
  t = fresh(); s = app_speech_start(&t); assert(s);
  char chinese[9005]; chinese[0] = 'x';
  for (unsigned i = 0; i < 3000; ++i) memcpy(chinese + 1 + i * 3, "中", 3);
  strcpy(chinese + 9001, "。");
  assert(app_speech_append(s, chinese, strlen(chinese)) == ESP_OK);
  assert(finish(s, &t, true) == ESP_OK && calls == 1 && !strcmp(played, chinese));
  t = fresh(); s = app_speech_start(&t); assert(s);
  char too_long[APP_REPLY_TEXT_BYTES]; memset(too_long, 'a', sizeof(too_long));
  assert(app_speech_append(s, too_long, sizeof(too_long)) == ESP_ERR_INVALID_SIZE);
  assert(finish(s, &t, false) == ESP_ERR_INVALID_STATE);
  fail_task = true; assert(!app_speech_start(&t));
  puts("speech bounded queue, concurrent receive, UTF-8, batching, cancellation, failure and join: PASS");
}
