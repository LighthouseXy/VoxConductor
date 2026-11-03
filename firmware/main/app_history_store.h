#pragma once
#include "app_conversation.h"
#include "app_turn.h"

// 标准文件接口；启动恢复和语音任务串行调用，不在音频回调中写盘。
// 恢复返回：1=已恢复，0=没有快照，-1=文件损坏/读取失败。
int app_history_restore(const char *directory, app_conversation_t *history);
typedef enum {
  APP_COMMIT_COMMITTED, APP_COMMIT_RETRYABLE_FAILURE,
  APP_COMMIT_PERMANENT_FAILURE, APP_COMMIT_UNCERTAIN
} app_commit_status_t;
typedef struct {
  app_commit_status_t status;
  unsigned revision;
  const char *reason;
} app_commit_result_t;
app_commit_result_t app_history_commit(const char *directory, const app_conversation_t *history);

bool app_history_save(const char *directory, const app_conversation_t *history);
bool app_history_event(const char *directory, const app_turn_t *turn,
                       const app_turn_event_t *event, long long utc_seconds);
cJSON *app_history_search(const char *directory, const char *keyword);
cJSON *app_history_query(const char *directory, const char *keyword, long before_offset, const char *record_id);
typedef struct { long long from_utc, to_utc; const char *turn_id, *role; } app_history_filter_t;
cJSON *app_history_query_filtered(const char *directory, const char *keyword, long before_offset,
                                  const char *record_id, const app_history_filter_t *filter);
// 空闲时最多读取约32KiB原文，生成可重建的定位索引；不修改原文。
bool app_history_index_tick(const char *directory);
bool app_history_erase(const char *directory, const app_conversation_t *replacement);
// 定向删除已关联的原文轮次，失效派生索引/旧快照；失败保留删除屏障。
bool app_history_forget(const char *directory, const app_conversation_t *replacement, const cJSON *notes);
bool app_history_retry_forget(const char *directory, app_conversation_t *history);
bool app_history_deletion_pending(const char *directory);
// 当前检索卷8MiB，最多另存4个封存卷；不覆盖旧卷、不自动删除。
bool app_history_capacity(const char *directory,unsigned *active_bytes,unsigned *archives);

cJSON *app_history_recent(const char *directory,long before_offset);

// NULL expected: read only. Otherwise compare before updating generation/unfencing.
// Serial coordinator callers only; any unreadable existing slot blocks reconciliation.
cJSON *app_history_task_receipt(const char *directory, const cJSON *expected);
