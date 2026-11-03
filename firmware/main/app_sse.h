#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "cJSON.h"
#include "app_conversation.h"

typedef struct {
  char text[APP_REPLY_TEXT_BYTES];
  struct { char id[128], name[49], arguments[513]; } tools[4];
  unsigned tool_count;
  bool done, stopped, speech_committed;
  char finish[32];
} app_sse_t;
// 一条完整SSE data行；只需重组正文/工具时使用。
bool app_sse_event(app_sse_t *s, const char *data);
// 同时读取usage/错误详情时借用同一次解析；无论成功失败，调用者都须删除*event。
// [DONE]没有JSON，*event为NULL；字符串完整性与嵌入NUL仍由此入口校验。
bool app_sse_event_with_json(app_sse_t *s, const char *data, cJSON **event);
cJSON *app_sse_tool_message(const app_sse_t *s);
// 纯文本分支可增量发布；发布后拒绝转为工具分支，避免边播报边执行。
bool app_sse_speech_ready(const app_sse_t *s, bool tools_enabled);
