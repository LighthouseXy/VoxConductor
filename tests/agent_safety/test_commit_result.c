#include "app_history_store.h"
#include "esp_err.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_memory_alloc.h"
enum { ESP_OK = 0, ESP_FAIL = -1, ESP_ERR_INVALID_ARG = 0x102 };
static app_conversation_t *s_history;
static const char *s_source_turn = "synthetic";
static int64_t s_summary_next_us;
static app_commit_status_t status;
static int64_t esp_timer_get_time(void) { return 10; }
static app_commit_result_t app_storage_save_context(const app_conversation_t *h) {
  assert(h == s_history);
  return (app_commit_result_t){status, 3, "synthetic"};
}
#include "commit_function.inc"
int main(void) {
  for (status = APP_COMMIT_COMMITTED; status <= APP_COMMIT_UNCERTAIN; ++status) {
    s_history = calloc(1, sizeof(*s_history)); assert(s_history);
    s_summary_next_us = 0;
    esp_err_t result = app_cloud_commit_turn("question", "answer");
    assert((result == ESP_OK) == (status == APP_COMMIT_COMMITTED));
    assert((s_summary_next_us != 0) == (status == APP_COMMIT_COMMITTED));
    s_summary_next_us = 0;
    app_cloud_remember_interrupted("interrupted question", "unplayed answer", 0);
    assert((s_summary_next_us != 0) == (status == APP_COMMIT_COMMITTED));
    assert(s_history->count == 2);
    assert(strstr(s_history->turns[1].assistant, "没有完整回答已播放"));
    assert(!strstr(s_history->turns[1].assistant, "unplayed answer"));
    free(s_history);
  }
  puts("PASS all four commit statuses propagate; normal and interrupted saves: only committed advances summary clock");
}
