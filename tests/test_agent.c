#include "app_agent.h"
#include "app_control.h"
#include "app_devices.h"
#include "app_history_store.h"
#include "app_weather.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int64_t now_us = 1000000;
static bool writable = true;
static unsigned writes, actions;
// Platform control is tested separately; keep this native Agent fixture isolated.
cJSON *app_control_voice_context(void) { return cJSON_Parse("{\"data_type\":\"device_snapshot_not_user_instruction\"}"); }
bool app_control_voice_pending(void) { return false; }
void app_control_voice_cancel(void) {}
char *app_control_voice_pending_context(void) { return NULL; }
char *app_control_voice_command(const char *text) { (void)text; return NULL; }
bool app_control_voice_tool(const char *name) {
  return name && (!strcmp(name,"get_local_state") || !strcmp(name,"set_local_setting") ||
    !strcmp(name,"control_media") || !strcmp(name,"control_desktop") || !strcmp(name,"manage_anniversary"));
}
char *app_control_voice_execute(const char *name,const char *args,const char *text) {
  (void)name; (void)args; (void)text; return strdup("{\"error\":\"platform_stub\"}");
}
cJSON *app_control_voice_tools(void) {
  const char *names[] = {"get_local_state","set_local_setting","control_media","control_desktop","manage_anniversary"};
  cJSON *tools = cJSON_CreateArray();
  for (unsigned i=0;i<5;++i) {
    cJSON *tool=cJSON_CreateObject(), *fn=cJSON_AddObjectToObject(tool,"function");
    cJSON *parameters=cJSON_AddObjectToObject(fn,"parameters");
    cJSON_AddObjectToObject(parameters,"properties");
    cJSON_AddArrayToObject(parameters,"required");
    cJSON_AddStringToObject(fn,"name",names[i]); cJSON_AddItemToArray(tools,tool);
  }
  return tools;
}
uint32_t esp_random(void) {
  static uint32_t id;
  return ++id;
}
int64_t esp_timer_get_time(void) { return now_us; }
bool app_network_weather_snapshot(app_weather_t *out, unsigned *age) {
  (void)out;
  (void)age;
  return false;
}
bool app_storage_persist_context(const app_conversation_t *h) {
  (void)h;
  ++writes;
  return writable;
}
cJSON *app_storage_task_receipt(const cJSON *expected) { (void)expected; return cJSON_CreateArray(); }
bool app_storage_erase_history(const app_conversation_t *h) {
  (void)h;
  return writable;
}
bool app_storage_forget_memory(const app_conversation_t *h,
                               const cJSON *notes) {
  (void)h;
  assert(cJSON_GetArraySize(notes));
  return writable;
}
bool app_storage_retry_forget(app_conversation_t *h) {
  (void)h;
  return writable;
}
bool app_storage_deletion_pending(void) { return false; }
cJSON *app_storage_search_history(const char *k) {
  (void)k;
  return cJSON_Parse("{\"matches\":[]}");
}
cJSON *app_storage_history_query(const char *k, long before, const char *id) {
  (void)before;
  (void)id;
  return app_storage_search_history(k);
}
cJSON *app_storage_history_query_filtered(const char *k, long before,
                                          const char *id,
                                          const app_history_filter_t *filter) {
  assert(!filter->role || !strcmp(filter->role, "user") ||
         !strcmp(filter->role, "assistant"));
  return app_storage_history_query(k, before, id);
}
bool app_device_configured(const char *id) { return !strcmp(id, "light"); }
cJSON *app_device_ids(void) { return cJSON_Parse("[\"light\"]"); }
char *app_device_request(const char *id, const char *command, bool value) {
  (void)id;
  (void)value;
  if (command)
    ++actions;
  return strdup("{\"status\":\"acknowledged\"}");
}
static void contains(char *s, const char *needle) {
  assert(s && strstr(s, needle));
  free(s);
}
// Native fixtures exercise argument validation, not the model's intent choice.
static char *execute(const char *name, const char *arguments, const char *text, const app_turn_t *turn) {
  cJSON *args = cJSON_Parse(arguments);
  if (app_agent_write_tool(name)) cJSON_AddStringToObject(args, "request", text);
  char *json = cJSON_PrintUnformatted(args); cJSON_Delete(args);
  char *reply = app_agent_execute(name, json, text, turn); free(json);
  return reply;
}
int main(void) {
  // 北京时间2024-01-01 16:59，未说早晚取最近未来时刻。
  const long long afternoon = 1704067200LL + 8 * 3600 + 59 * 60;
  assert(app_agent_clock_due(afternoon, 5, 2, "auto", -1) == afternoon + 180);
  assert(app_agent_clock_due(afternoon + 240, 5, 2, "auto", -1) ==
         afternoon + 180 + 12 * 3600);
  assert(app_agent_clock_due(afternoon, 5, 2, "pm", -1) == afternoon + 180);
  assert(!app_agent_clock_due(afternoon, 5, 2, "am", -1));
  assert(app_agent_clock_due(afternoon, 5, 2, "am", 1) ==
         afternoon + 180 + 12 * 3600);
  assert(!app_agent_clock_due(afternoon + 180, 17, 2, "24h", 0));
  assert(!app_agent_clock_due(afternoon, 25, 0, "auto", -1));
  assert(!app_agent_clock_due(afternoon, 5, 60, "auto", -1));
  assert(!app_agent_clock_due(0, 5, 2, "auto", -1));
  assert(app_agent_clock_rule_due(afternoon, 5, 2, "auto", -1, "weekly",
                                  "每周一五点零二分提醒我喝水") ==
         afternoon + 180);
  assert(!app_agent_clock_rule_due(afternoon, 5, 2, "auto", -1, "weekly",
                                   "每周一和每周五提醒我喝水"));
  assert(app_agent_clock_rule_due(afternoon, 5, 2, "auto", -1, "weekly",
                                  "每周五五点零二分提醒我喝水") ==
         afternoon + 4 * 86400 - 12 * 3600 + 180);
  const long long saturday_afternoon = afternoon + 5 * 86400;
  assert(app_agent_clock_rule_due(saturday_afternoon, 5, 2, "auto", -1,
                                  "weekdays", "工作日五点零二分提醒我喝水") ==
         saturday_afternoon + 2 * 86400 - 12 * 3600 + 180);
  app_conversation_t *h = calloc(1, sizeof(*h));
  assert(h);
  unsigned capabilities = app_agent_tool_mask();
  assert(capabilities & APP_TOOL_WEATHER);
  assert(capabilities & APP_TOOL_LOCAL_WRITE);
  assert(app_agent_tool_allowed(capabilities, "prepare_device"));
  assert(!app_agent_tool_allowed(capabilities, "shell"));
  h->count = 0;
  cJSON *selected = app_agent_select_tools(APP_TOOL_WEATHER);
  assert(selected && cJSON_GetArraySize(selected) == 1);
  const cJSON *f = cJSON_GetObjectItemCaseSensitive(
      cJSON_GetArrayItem(selected, 0), "function");
  assert(!strcmp(cJSON_GetObjectItemCaseSensitive(f, "name")->valuestring,
                 "get_weather"));
  cJSON_Delete(selected);
  assert(!app_agent_tool_allowed(APP_TOOL_WEATHER, "search_history"));
  assert(!app_agent_tool_allowed(0, "prepare_task"));
  assert(!app_agent_tool_allowed(~0u, "shell"));
  app_turn_t turn = {0};
  strcpy(turn.id, "test-1");
  assert(!app_agent_command("", h));
  cJSON *tools = app_agent_tools();
  assert(cJSON_GetArraySize(tools) == 14);
  assert(app_agent_tool_allowed(APP_TOOL_LOCAL_READ, "get_local_state"));
  assert(!app_agent_tool_allowed(APP_TOOL_LOCAL_READ, "control_media"));
  assert(app_agent_tool_allowed(APP_TOOL_LOCAL_WRITE, "control_media"));
  cJSON_Delete(tools);
  contains(execute("get_weather", "{\"city\":\"北京\"}", "", &turn),
           "unsupported_city");
  contains(execute("shell", "{}", "", &turn), "unknown_tool");
  const char *args = "{\"key\":\"饮品\",\"value\":\"茶\",\"valid_seconds\":0}";
  contains(app_agent_execute("prepare_memory", args, "你好", &turn),
           "current_turn_request_required");
  contains(execute("prepare_memory", args, "记住我喜欢茶", &turn),
           "pending_not_saved");
  assert(!h->notes[0] && writes == 0);
  contains(app_agent_command("确认保存。", h), "已保存");
  assert(strstr(h->notes, "茶"));
  assert(strstr(h->notes, "memory_id") && strstr(h->notes, "category"));
  cJSON *saved = cJSON_Parse(h->notes);
  char id[64];
  strcpy(id, cJSON_GetObjectItem(cJSON_GetArrayItem(saved, 0), "memory_id")
                 ->valuestring);
  cJSON_Delete(saved);
  contains(execute(
               "prepare_memory",
               "{\"key\":\"喜欢的饮料\",\"value\":\"茶\",\"valid_seconds\":0}",
               "修改记忆", &turn),
           "pending_not_saved");
  contains(app_agent_command("确认保存", h), "已保存");
  saved = cJSON_Parse(h->notes);
  assert(cJSON_GetArraySize(saved) == 1);
  assert(
      !strcmp(id, cJSON_GetObjectItem(cJSON_GetArrayItem(saved, 0), "memory_id")
                      ->valuestring));
  cJSON_Delete(saved);
  contains(execute(
               "prepare_memory",
               "{\"key\":\"饮品\",\"value\":\"咖啡\",\"valid_seconds\":0}",
               "修改记忆", &turn),
           "pending_not_saved");
  writable = false;
  contains(app_agent_command("确认保存", h), "失败");
  assert(!strstr(h->notes, "咖啡"));
  writable = true;
  contains(execute("prepare_memory", args, "记住", &turn),
           "pending_not_saved");
  now_us += 121000000;
  contains(app_agent_command("确认保存", h), "没有有效");
  contains(execute("prepare_memory", args, "记住", &turn),
           "pending_not_saved");
  app_agent_cancel_pending();
  contains(app_agent_command("确认保存", h), "没有有效");
  const char *memo =
      "{\"kind\":\"note\",\"title\":\"滤芯型号A15\",\"delay_seconds\":0}";
  unsigned before_memo = writes;
  contains(execute("prepare_task", memo, "添加备忘录，滤芯型号A15", &turn),
           "pending_not_executed");
  assert(writes == before_memo);
  contains(app_agent_command("确认执行", h), "已保存");
  assert(writes == before_memo + 1);
  assert(strstr(h->tasks, "\"kind\":\"note\"") &&
         strstr(h->tasks, "\"due_utc\":0"));
  contains(app_agent_command("查看备忘录", h), "滤芯型号A15");
  contains(app_agent_command("确认执行", h), "没有有效");
  app_task_notice_t memo_notice;
  assert(!app_agent_due_notice(&memo_notice)); // Notes never sound on a timer.
  h->tasks[0] = 0;
  writable = false;
  contains(execute("prepare_task", memo, "添加备忘录，滤芯型号A15", &turn),
           "pending_not_executed");
  contains(app_agent_command("确认执行", h), "SD写入失败");
  assert(!h->tasks[0]); // Failed SD write rolls back.
  writable = true;
  contains(app_agent_execute("prepare_task", memo, "不要添加备忘录", &turn),
           "error");
  assert(!h->tasks[0]);
  contains(execute("prepare_task", memo, "添加备忘录，快递码1234", &turn),
           "error");
  assert(!h->tasks[0]); // Never substitute unrelated content.
  contains(execute(
               "prepare_task",
               "{\"kind\":\"todo\",\"title\":\"买牛奶\",\"delay_seconds\":0}",
               "添加待办", &turn),
           "pending_not_executed");
  assert(!h->tasks[0]);
  contains(app_agent_command("确认执行", h), "已保存");
  contains(app_agent_command("查看待办", h), "买牛奶：待处理");
  contains(
      execute(
          "prepare_task",
          "{\"kind\":\"complete\",\"title\":\"买牛奶\",\"delay_seconds\":0}",
          "完成待办", &turn),
      "pending_not_executed");
  contains(app_agent_command("确认执行", h), "已保存");
  contains(execute("prepare_device",
                             "{\"device_id\":\"light\",\"value\":true}",
                             "打开灯", &turn),
           "pending_not_executed");
  assert(!actions);
  contains(app_agent_command("确认执行", h), "确认执行");
  assert(actions == 1);
  contains(app_agent_command("确认执行", h), "没有有效");
  assert(actions == 1);
  assert(app_conversation_add(h, "你好", "你好"));
  contains(app_agent_command("开始新对话", h), "已开始");
  assert(h->count == 0 && strstr(h->notes, "茶"));
  contains(app_agent_command("删除记忆：饮品", h), "将删除");
  contains(app_agent_command("确认清空", h), "已完成");
  assert(!strstr(h->notes, "茶"));
  contains(app_agent_command("清空全部记忆", h), "将清空");
  contains(app_agent_command("确认清空", h), "已完成");
  assert(!h->notes[0] && !h->tasks[0]);
  const char *alarm =
      "{\"kind\":\"alarm\",\"title\":\"喝水\",\"delay_seconds\":60}";
  contains(app_agent_execute("prepare_task", alarm, "不要提醒我喝水", &turn),
           "current_turn_request_required");
  assert(!h->tasks[0]);
  contains(execute("search_history",
                             "{\"keyword\":\"咖啡\",\"role\":\"assistant\","
                             "\"from_utc\":1704067200,\"to_utc\":1704153600}",
                             "查原话", &turn),
           "matches");
  contains(execute("search_history",
                             "{\"keyword\":\"咖啡\",\"role\":\"system\"}",
                             "查原话", &turn),
           "invalid_keyword");
  contains(execute("search_history",
                             "{\"keyword\":\"咖啡\",\"from_utc\":1704153600,"
                             "\"to_utc\":1704067200}",
                             "查原话", &turn),
           "invalid_keyword");
  contains(
      execute("prepare_task", alarm, "五点零二分提醒我喝水", &turn),
      "提醒未设置");
  assert(!h->tasks[0]);
  contains(
      execute(
          "prepare_task",
          "{\"kind\":\"alarm\",\"title\":\"喝水\",\"delay_seconds\":0,"
          "\"clock\":{\"hour\":5,\"minute\":2,\"period\":\"am\",\"day\":1}}",
          "明天上午五点零二分提醒我喝水", &turn),
      "pending_not_executed");
  contains(app_agent_command("确认执行", h), "已保存");
  assert(strstr(h->tasks, "喝水"));
  h->tasks[0] = 0;
  writable = false;
  contains(
      execute("prepare_task", alarm, "一分钟后提醒我喝水", &turn),
      "pending_not_executed");
  contains(app_agent_command("确认执行", h), "SD写入失败");
  assert(!h->tasks[0]);
  contains(app_agent_command("确认执行", h), "没有有效");
  writable = true;
  contains(
      execute("prepare_task", alarm, "一分钟后提醒我喝水", &turn),
      "pending_not_executed");
  contains(app_agent_command("确认执行", h), "已保存");
  assert(strstr(h->tasks, "喝水") && strstr(h->tasks, "pending"));
  assert(!app_agent_due_task());
  contains(app_agent_command("确认执行", h), "没有有效");
  snprintf(h->tasks, sizeof(h->tasks),
           "[{\"id\":\"alarm-1\",\"kind\":\"alarm\",\"title\":\"喝水\","
           "\"status\":\"pending\",\"due_utc\":%lld}]",
           (long long)time(NULL) - 1);
  writable = false;
  assert(!app_agent_due_task());
  assert(strstr(h->tasks, "pending"));
  writable = true;
  contains(app_agent_due_task(), "喝水");
  assert(!app_agent_due_task());
  snprintf(h->tasks, sizeof(h->tasks),
           "[{\"kind\":\"alarm\",\"title\":\"喝水\",\"status\":\"pending\","
           "\"due_utc\":%lld}]",
           (long long)time(NULL) - 301);
  assert(!app_agent_due_task());
  assert(strstr(h->tasks, "missed"));
  // 周五09:00 -> 周一09:00，独立于主机当前时区。
  const long long friday = 1704416400;
  assert(app_agent_next_repeat(friday, friday, "weekdays") ==
         friday + 3 * 86400);
  assert(app_agent_next_repeat(friday, friday, "weekly") == friday + 7 * 86400);
  assert(app_agent_next_repeat(friday, friday + 5 * 86400, "daily") ==
         friday + 6 * 86400);
  assert(!app_agent_next_repeat(friday, friday, "once"));
  snprintf(h->tasks, sizeof(h->tasks),
           "[{\"id\":\"a\",\"kind\":\"alarm\",\"title\":\"同名\",\"status\":"
           "\"pending\",\"repeat\":\"daily\",\"due_utc\":%lld},{\"id\":\"b\","
           "\"kind\":\"alarm\",\"title\":\"同名\",\"status\":\"pending\",\"due_"
           "utc\":%lld}]",
           (long long)time(NULL) - 1, (long long)time(NULL) + 1000);
  app_task_view_t rows[8];
  assert(app_agent_task_snapshot(rows) == 2);
  app_task_notice_t notice;
  assert(app_agent_due_notice(&notice));
  assert(!strcmp(notice.id, "a"));
  assert(app_agent_task_result(&notice, "failed"));
  assert(app_agent_task_snapshot(rows) == 2);
  assert(!strcmp(rows[0].status, "pending") &&
         !strcmp(rows[0].last_result, "failed"));
  assert(!app_agent_due_notice(&notice));
  char before_change[4096];
  strcpy(before_change, h->tasks);
  writable = false;
  assert(!app_agent_task_change("b", "delete", 0, NULL, NULL));
  assert(!strcmp(before_change, h->tasks));
  writable = true;
  assert(app_agent_task_change("b", "cancel", 0, NULL, NULL));
  assert(app_agent_task_snapshot(rows) == 2);
  assert(!strcmp(rows[0].status, "pending") &&
         !strcmp(rows[1].status, "cancelled"));
  assert(app_agent_task_change("a", "snooze", 300, NULL, NULL));
  assert(app_agent_task_snapshot(rows) == 2);
  assert(rows[0].due_utc > time(NULL) + 290 &&
         strstr(h->tasks, "repeat_anchor_utc"));
  assert(!app_agent_task_change("a", "not_cancel", 0, NULL, NULL));
  assert(app_agent_task_change("b", "delete", 0, NULL, NULL));
  assert(app_agent_task_snapshot(rows) == 1);
  app_agent_cancel_pending();
  const char *change = "{\"task_id\":\"a\",\"action\":\"delete\",\"delay_"
                       "seconds\":0,\"title\":\"\",\"repeat\":\"daily\"}";
  contains(
      app_agent_execute("prepare_task_change", change, "不删除同名任务", &turn),
      "current_turn_request_required");
  contains(
      execute("prepare_task_change", change, "删除同名任务", &turn),
      "pending_not_executed");
  app_agent_cancel_pending();
  // 未来周六改成工作日，首轮必须落在周一，不能等第二轮才跳过周末。
  time_t local = (time_t)(time(NULL) + 28800);
  struct tm current;
  gmtime_r(&local, &current);
  long long saturday = (long long)time(NULL) +
                       (6 - current.tm_wday + 7) % 7 * 86400LL + 7 * 86400LL;
  snprintf(h->tasks, sizeof(h->tasks),
           "[{\"id\":\"weekend\",\"kind\":\"alarm\",\"title\":\"站起来\","
           "\"status\":\"pending\",\"repeat\":\"daily\",\"due_utc\":%lld}]",
           saturday);
  assert(app_agent_task_change("weekend", "edit", 0, NULL, "weekdays"));
  assert(app_agent_task_snapshot(rows) == 1 &&
         rows[0].due_utc == saturday + 2 * 86400LL);
  assert(app_agent_task_change("weekend", "complete", 0, NULL, NULL));
  assert(app_agent_task_change("weekend", "edit", 0, NULL, "daily"));
  assert(app_agent_task_snapshot(rows) == 1 &&
         !strcmp(rows[0].status, "pending"));
  // Local task checkboxes can undo completion, without re-sounding old alarms.
  strcpy(h->tasks, "[{\"id\":\"todo-ui\",\"kind\":\"todo\",\"title\":\"读书\","
                   "\"status\":\"completed\",\"due_utc\":0}]");
  strcpy(before_change, h->tasks);
  writable = false;
  assert(!app_agent_task_change("todo-ui", "reopen", 0, NULL, NULL));
  assert(!strcmp(before_change, h->tasks));
  writable = true;
  assert(app_agent_task_change("todo-ui", "reopen", 0, NULL, NULL));
  assert(app_agent_task_snapshot(rows) == 1 &&
         !strcmp(rows[0].status, "pending"));
  assert(!app_agent_task_change("todo-ui", "reopen", 0, NULL, NULL));
  snprintf(h->tasks, sizeof(h->tasks),
           "[{\"id\":\"expired-ui\",\"kind\":\"alarm\",\"title\":\"喝水\","
           "\"status\":\"completed\",\"due_utc\":%lld}]",
           (long long)time(NULL) - 1);
  strcpy(before_change, h->tasks);
  assert(!app_agent_task_change("expired-ui", "reopen", 0, NULL, NULL));
  assert(!strcmp(before_change, h->tasks));
  snprintf(h->tasks, sizeof(h->tasks),
           "[{\"id\":\"future-ui\",\"kind\":\"alarm\",\"title\":\"喝水\","
           "\"status\":\"completed\",\"due_utc\":%lld}]",
           (long long)time(NULL) + 1000);
  assert(app_agent_task_change("future-ui", "reopen", 0, NULL, NULL));
  strcpy(h->notes, "[{\"key\":\"位置\",\"value\":\"杭州\",\"valid_until\":1}]");
  cJSON *messages = app_conversation_messages(h, "你好");
  assert(messages && cJSON_GetArraySize(messages) == 3);
  cJSON_Delete(messages);
  app_agent_call_budget_t budget = {0};
  assert(app_agent_accept_call(&budget, "search_history",
                               "{\"keyword\":\"咖啡\"}"));
  assert(!app_agent_accept_call(&budget, "search_history",
                                "{ \"keyword\" : \"咖啡\" }"));
  assert(app_agent_accept_call(&budget, "search_history",
                               "{\"keyword\":\"饮品\"}"));
  assert(
      app_agent_accept_call(&budget, "prepare_memory", "{\"key\":\"饮品\"}"));
  assert(
      !app_agent_accept_call(&budget, "prepare_memory", "{\"key\":\"饮品\",\"value\":\"别的\"}"));
  assert(app_agent_accept_call(&budget, "get_device",
                               "{\"device_id\":\"light\"}"));
  assert(!app_agent_accept_call(&budget, "get_time", "{}"));
  budget = (app_agent_call_budget_t){0};
  assert(app_agent_accept_call(&budget, "get_weather", "{\"city\":\"杭州\"}"));
  assert(!app_agent_accept_call(&budget, "get_weather",
                                "{ \"city\" : \"杭州\" }"));
  assert(app_agent_accept_call(&budget, "get_weather", "{\"city\":\"苏州\"}"));
  assert(app_agent_accept_call(&budget, "get_device",
                               "{\"device_id\":\"light\"}"));
  assert(app_agent_accept_call(&budget, "get_device",
                               "{\"device_id\":\"socket\"}"));
  assert(!app_agent_accept_call(&budget, "get_device",
                                "{\"device_id\":\"another\"}"));
  budget = (app_agent_call_budget_t){0};
  assert(app_agent_accept_call(
      &budget, "search_history",
      "{\"keyword\":\"咖啡\",\"role\":\"user\",\"from_utc\":1704067200}"));
  assert(!app_agent_accept_call(
      &budget, "search_history",
      "{\"from_utc\":1704067200,\"role\":\"user\",\"keyword\":\"咖啡\"}"));
  assert(app_agent_accept_call(
      &budget, "search_history",
      "{\"keyword\":\"咖啡\",\"role\":\"assistant\",\"from_utc\":1704067200}"));
  assert(app_agent_accept_call(
      &budget, "search_history",
      "{\"keyword\":\"咖啡\",\"role\":\"assistant\",\"from_utc\":1704153600}"));
  contains(app_agent_command("重试删除", h), "没有可重试");
  // Native clock alarm creation: no LLM or additional confirmation round.
  strcpy(h->tasks, "[]");
  assert(!app_agent_create_alarm(24, 0, "once"));
  assert(!app_agent_create_alarm(0, 60, "once"));
  assert(!app_agent_create_alarm(0, 0, "unknown"));
  writable = false;
  assert(!app_agent_create_alarm(0, 0, "once"));
  assert(!strcmp(h->tasks, "[]"));
  writable = true;
  assert(app_agent_create_alarm(0, 0, "once"));
  assert(app_agent_create_alarm(12, 34, "daily"));
  assert(app_agent_create_alarm(8, 0, "weekdays"));
  assert(app_agent_task_snapshot(rows) == 3);
  assert(strcmp(rows[0].id, rows[1].id) && strcmp(rows[1].id, rows[2].id));
  assert(!strcmp(rows[0].kind, "alarm") && !strcmp(rows[0].status, "pending"));
  for (unsigned i = 0; i < 3; ++i)
    assert(rows[i].due_utc > (long long)time(NULL));
  for (unsigned i = 3; i < 8; ++i)
    assert(app_agent_create_alarm(i, 0, "once"));
  strcpy(before_change, h->tasks);
  assert(!app_agent_create_alarm(9, 0, "once"));
  assert(!strcmp(before_change, h->tasks));
  free(h);
  puts("agent confirmation, expiry, rollback, tasks and device guards: PASS");
}
