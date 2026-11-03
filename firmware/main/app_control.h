#pragma once
#include "cJSON.h"
#include <stdbool.h>
#include <stdint.h>
// One bounded web job; only the existing voice-exclusive coordinator executes
// it.
bool app_control_init(void);
uint32_t app_control_submit(const char *json);
// 返回对象由调用者cJSON_Delete；submit返回0表示未接受，不能继续等待虚构ID。
// DONE只说明作业结束；仍需核对result.ok/applied/saved，不能当作操作成功。
cJSON *app_control_status(uint32_t id);
// Read-only RAM snapshot: no task/history SD reads and no queued mutation.
cJSON *app_control_snapshot(void);
void app_control_service(void);
// Voice calls run under the voice coordinator's exclusive ownership, never
// submit a web job and wait for that same coordinator to service itself.
cJSON *app_control_voice_tools(void);
cJSON *app_control_voice_context(void);
bool app_control_voice_tool(const char *name);
char *app_control_voice_execute(const char *name, const char *arguments,
                                const char *user_text);
char *app_control_voice_command(const char *text);
// Only for a locally accepted interruption; returns true for a stop-only phrase.
bool app_control_strip_interrupt(char *text);
// Whole-utterance local exit only; never a substring/LLM decision.
bool app_control_end_session(const char *text);
char *app_control_voice_pending_context(void);
bool app_control_voice_pending(void);
void app_control_voice_cancel(void);
// Actual readback receipt, not the model's success claim; consumed at turn end.
bool app_control_voice_home_applied(void);
// Called after each voice result. Failed replies may reveal an already applied
// home setting, but never run deferred actions or override explicit cancellation.
bool app_control_voice_finish(bool success);
