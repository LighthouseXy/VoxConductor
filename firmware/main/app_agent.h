#pragma once
#include "app_conversation.h"
#include "app_turn.h"
// UTC秒输入/输出；day=-1未指定，0今天，1明天，2后天。
long long app_agent_clock_due(long long now, int hour, int minute, const char *period, int day);
// 创建与修改共用：先确定循环日期，再按原话时段选择最近未来钟点。
long long app_agent_clock_rule_due(long long now, int hour, int minute, const char *period,
                                  int day, const char *repeat, const char *text);

// 工具仅在单语音任务中执行；UI/网络回调不得直接调用。
cJSON *app_agent_tools(void);
enum {
  APP_TOOL_TIME = 1u, APP_TOOL_WEATHER = 2u, APP_TOOL_HISTORY = 4u,
  APP_TOOL_MEMORY = 8u, APP_TOOL_TASK = 16u, APP_TOOL_DEVICE = 32u,
  APP_TOOL_DEVICE_WRITE = 64u,
  APP_TOOL_LOCAL_READ = 128u, APP_TOOL_LOCAL_WRITE = 256u,
};
// Capability allow-list, never a vocabulary/intent classifier.
unsigned app_agent_tool_mask(void);
bool app_agent_tool_allowed(unsigned mask, const char *name);
bool app_agent_write_tool(const char *name);
// Provenance only, not proof of semantic consent. Destructive writes still confirm.
bool app_agent_request_matches(const cJSON *args, const char *current_user);
char *app_agent_runtime_context(void);
cJSON *app_agent_select_tools(unsigned mask);
typedef struct { char signatures[4][640]; unsigned count; } app_agent_call_budget_t;
// 查询按语义参数去重；本机写按设置项/动作/槽位去重，其余按能力去重。
bool app_agent_accept_call(app_agent_call_budget_t *budget, const char *name, const char *arguments);
char *app_agent_execute(const char *name, const char *arguments,
                        const char *user_text, const app_turn_t *turn);
// 明确的本地管理命令；返回非NULL时不调用LLM。调用者free。
char *app_agent_command(const char *text, app_conversation_t *history);
// 调用者必须持有与语音入口相同的独占权；返回提醒标题，由调用者free。
char *app_agent_due_task(void);
void app_agent_cancel_pending(void);
bool app_agent_has_pending(void);
typedef struct {
  char id[64], title[128], kind[24], status[32], repeat[16], last_result[32];
  long long due_utc;
} app_task_view_t;
typedef struct { char id[64], title[128]; long long due_utc; } app_task_notice_t;
unsigned app_agent_task_snapshot(app_task_view_t out[8]);
bool app_agent_task_change(const char *id, const char *action, int seconds,
                           const char *title, const char *repeat);
// 仅由与语音互斥的后台协调器调用；复用提醒存盘、到期与播报链路。
bool app_agent_create_local_task(const char *kind,const char *title,long long due_utc,const char *repeat);
bool app_agent_create_alarm(unsigned hour, unsigned minute, const char *repeat);
bool app_agent_due_notice(app_task_notice_t *notice);
bool app_agent_task_result(const app_task_notice_t *notice, const char *result);
long long app_agent_next_repeat(long long due, long long now, const char *repeat);
// 仅返回仍有效的RAM待确认动作，不从SD恢复旧授权。调用者free。
char *app_agent_pending_context(void);

// Coordinator owns this deadline; zero denotes an idle/background operation.
void app_agent_set_deadline_us(int64_t deadline);
