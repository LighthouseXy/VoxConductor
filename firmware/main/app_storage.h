#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "app_conversation.h"
#include "app_turn.h"
#include "app_history_store.h"

// 0=不写入、不恢复结构化文字记忆；不会删除卡上已有文件。
#ifndef APP_STORAGE_MEMORY_ENABLED
#define APP_STORAGE_MEMORY_ENABLED 1
#endif

void app_storage_record_event(const app_turn_t *turn, const app_turn_event_t *event);
void app_storage_restore_context(app_conversation_t *history);
app_commit_result_t app_storage_save_context(const app_conversation_t *history);
bool app_storage_persist_context(const app_conversation_t *history);
cJSON *app_storage_search_history(const char *keyword);
cJSON *app_storage_history_query(const char *keyword, long before_offset, const char *record_id);
cJSON *app_storage_history_query_filtered(const char *keyword, long before_offset,
                                          const char *record_id, const app_history_filter_t *filter);
void app_storage_index_tick(void);
bool app_storage_erase_history(const app_conversation_t *replacement);
bool app_storage_forget_memory(const app_conversation_t *replacement, const cJSON *notes);
bool app_storage_retry_forget(app_conversation_t *history);
bool app_storage_deletion_pending(void);
bool app_storage_capacity(unsigned *active_bytes,unsigned *archives);

// 挂载microSD。失败时不会格式化存储卡。
esp_err_t app_storage_init(void);

// 以追加方式保存一轮对话，不覆盖既有记录。
esp_err_t app_storage_append_conversation(const char *transcript,
                                          const char *answer);

cJSON *app_storage_recent_history(long before_offset);

cJSON *app_storage_task_receipt(const cJSON *expected);
