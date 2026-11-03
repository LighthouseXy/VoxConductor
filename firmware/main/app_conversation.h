#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"

#define APP_CONTEXT_TURNS 6
#define APP_CONTEXT_TEXT_BYTES 2048
#define APP_REPLY_TEXT_BYTES 16384 // 单轮生成硬上限；近期窗口仍只存有标记摘录。

typedef struct {
  char user[APP_CONTEXT_TEXT_BYTES];
  char assistant[APP_CONTEXT_TEXT_BYTES];
  char source_turn_id[64];
} app_conversation_turn_t;

typedef struct {
  app_conversation_turn_t turns[APP_CONTEXT_TURNS];
  size_t count;
  char summary[2048];
  char summary_pending[12288];
  unsigned summary_version;
  char notes[8192]; // JSON数组；长期条目不与最近六轮一起淘汰。
  char tasks[4096]; // 本地闹钟/待办，不直接注入普通对话。
  char discussion[3072]; // 有界活动讨论数据；执行授权由具体操作的确认流程管理。
} app_conversation_t;

typedef enum { APP_REPLY_SHORT, APP_REPLY_EXPLAIN, APP_REPLY_DETAIL } app_reply_mode_t;
app_reply_mode_t app_conversation_reply_mode(const char *text);
unsigned app_conversation_reply_tokens(const char *text);
bool app_conversation_followup(const char *text);
const char *app_conversation_memory_key(const char *key);
const char *app_conversation_memory_category(const char *key);
// 按明确来源或原文名称/值/别名匹配关联。
bool app_conversation_memory_matches(const cJSON *note, const char *text, const char *source);
// 空闲摘要请求的结构化候选，只接收讨论字段，不保存任何执行授权。
bool app_conversation_refine_discussion(app_conversation_t *history, const cJSON *candidate);
bool app_conversation_forget(app_conversation_t *history, const cJSON *notes);

bool app_conversation_add(app_conversation_t *history, const char *user,
                          const char *assistant);
bool app_conversation_add_source(app_conversation_t *history, const char *user,
                                 const char *assistant, const char *source_turn_id);
// 输出回答策略、相关记忆/讨论状态、最近六轮和本轮 user。调用者 cJSON_Delete()。
cJSON *app_conversation_messages(const app_conversation_t *history,
                                  const char *user);
