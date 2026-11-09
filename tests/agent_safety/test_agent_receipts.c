#define main old_agent_main
#define app_storage_persist_context old_persist
#define app_device_request old_device
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include BASELINE_TEST_PATH
#pragma GCC diagnostic pop
#undef app_device_request
#undef app_storage_persist_context
#undef main

static app_conversation_t durable;
static unsigned saves, sent;
static bool storage = true, lost, fail_after_ack;
bool app_storage_persist_context(const app_conversation_t *h) {
  ++saves;
  if (!storage) return false;
  durable = *h;
  return true;
}
char *app_device_request(const char *id, const char *command, bool value) {
  (void)id; (void)value;
  assert(command && strstr(durable.tasks, command));
  assert(strstr(durable.tasks, "dispatch_committed"));
  ++sent;
  if (fail_after_ack) storage = false;
  return strdup(lost ? "{\"error\":\"execution_unknown\"}" : "{\"status\":\"acknowledged\"}");
}
static void prepare_device(app_conversation_t *h, const char *id) {
  app_turn_t turn = {0};
  snprintf(turn.id, sizeof(turn.id), "%s", id);
  app_agent_command("", h);
  contains(app_agent_execute("prepare_device", "{\"device_id\":\"light\",\"value\":true,\"request\":\"打开灯\"}",
                             "打开灯", &turn), "pending_not_executed");
}
static void reset(app_conversation_t *h) {
  app_agent_cancel_pending();
  app_agent_set_deadline_us(0);
  memset(h, 0, sizeof(*h)); memset(&durable, 0, sizeof(durable));
  storage = true; lost = fail_after_ack = false; saves = sent = 0; now_us = 1000000;
}
int main(void) {
  app_conversation_t *h = calloc(1, sizeof(*h)); assert(h);
  reset(h); prepare_device(h, "confirmed");
  contains(app_agent_command("确认执行", h), "已确认");
  assert(saves == 2 && sent == 1 && strstr(durable.tasks, "acknowledged"));
  assert(strstr(durable.tasks, "action_id") && strstr(durable.tasks, "operation_id"));
  contains(app_agent_command("确认执行", h), "没有有效"); assert(sent == 1);
  puts("PASS ACK persists a terminal receipt and repeat confirmation cannot resend");
  reset(h); prepare_device(h, "receipt-failure"); fail_after_ack = true;
  contains(app_agent_command("确认执行", h), "回执保存未确认");
  assert(saves == 2 && sent == 1 && strstr(durable.tasks, "dispatch_committed"));
  assert(strstr(h->tasks, "acknowledged"));
  puts("PASS known ACK remains in RAM and durable receipt failure is reported separately");
  reset(h); lost = true;
  for (unsigned i = 0; i < 8; ++i) {
    char id[32]; snprintf(id, sizeof(id), "unknown-%u", i); prepare_device(h, id);
    contains(app_agent_command("确认执行", h), "结果未知");
  }
  assert(sent == 8 && saves == 16 && strstr(durable.tasks, "outcome_unknown"));
  prepare_device(h, "unknown-8");
  contains(app_agent_command("确认执行", h), "未执行");
  assert(sent == 8 && strstr(durable.tasks, "unknown-0"));
  app_agent_cancel_pending(); *h = durable; app_agent_command("", h);
  contains(app_agent_command("确认执行", h), "没有有效"); assert(sent == 8);
  puts("PASS full unreconciled ledger refuses admission and restart simulation never replays");
  reset(h); prepare_device(h, "intent-failure"); storage = false;
  contains(app_agent_command("确认执行", h), "SD写入失败"); assert(sent == 0);
  puts("PASS failed write-ahead intent blocks dispatch");
  reset(h); prepare_device(h, "expired-turn"); app_agent_set_deadline_us(now_us);
  contains(app_agent_command("确认执行", h), "本轮已超时"); assert(sent == 0 && saves == 0);
  puts("PASS expired turn blocks write-ahead admission before any device effect");
  reset(h);
  strcpy(h->tasks, "[{\"id\":\"legacy-intent\",\"kind\":\"device\",\"title\":\"light\",\"status\":\"dispatch_committed\",\"due_utc\":0}]");
  app_agent_command("", h);
  assert(strstr(durable.tasks, "outcome_unknown") && sent == 0);
  contains(app_agent_command("查看设备回执", h), "待对账");
  contains(app_agent_command("确认执行", h), "没有有效"); assert(sent == 0);
  puts("PASS legacy unfinished intent recovers to visible unknown without restoring authority");
  free(h); return 0;
}
