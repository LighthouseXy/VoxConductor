/* Executes the repository's app_agent.c; all I/O is replaced by existing host
 * fixtures plus the two fault-injection stubs below. No device/network calls. */
#include "app_devices.h"
#include <unistd.h>
#include <sys/wait.h>
#define main baseline_agent_main
#define app_storage_persist_context baseline_persist_context
#define app_device_request baseline_device_request
/* The fixture's renamed main has C's implicit main-only return; it is not run. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include BASELINE_TEST_PATH
#pragma GCC diagnostic pop
#undef app_device_request
#undef app_storage_persist_context
#undef main

static app_conversation_t persisted;
static unsigned persist_calls, dispatches, gaps;
static bool storage_ok, lose_ack, fail_after_ack;

/* Deterministic UTC alongside the fixture's virtual monotonic now_us. */
time_t time(time_t *out) {
  const time_t now = 1704067200;
  if (out) *out = now;
  return now;
}

bool app_storage_persist_context(const app_conversation_t *h) {
  ++persist_calls;
  if (!storage_ok) return false;
  persisted = *h;
  return true;
}

char *app_device_request(const char *id, const char *command, bool value) {
  (void)value;
  assert(!strcmp(id, "light"));
  if (!command) return strdup("{\"status\":\"observed\",\"value\":true}");
  ++dispatches;
  /* A successful write-ahead commit must precede every simulated dispatch. */
  assert(strstr(persisted.tasks, command));
  assert(strstr(persisted.tasks, "dispatch_committed"));
  if (fail_after_ack) storage_ok = false;
  return strdup(lose_ack ?
      "{\"error\":\"execution_unknown_do_not_retry_automatically\"}" :
      "{\"status\":\"acknowledged\"}");
}

static void reset_case(app_conversation_t *h) {
  app_agent_cancel_pending();
  memset(h, 0, sizeof(*h));
  memset(&persisted, 0, sizeof(persisted));
  persist_calls = dispatches = 0;
  storage_ok = true;
  lose_ack = fail_after_ack = false;
  now_us = 1000000;
  assert(!app_agent_command("", h));
}

static void prepare(app_conversation_t *h, const char *id, const char *text) {
  (void)h;
  app_turn_t turn = {0};
  snprintf(turn.id, sizeof(turn.id), "%s", id);
  const char *args = "{\"device_id\":\"light\",\"value\":true,\"request\":\"打开灯\"}";
  contains(app_agent_execute("prepare_device", args, text, &turn),
           "pending_not_executed");
}

static void contract(const char *name, bool met) {
  printf("%s %s\n", met ? "PASS" : "UNMET", name);
  if (!met) ++gaps;
}

static const char *self_path;
static void task_confirmation_success(app_conversation_t *h) {
  const char *kinds[] = {"alarm", "note"};
  const char *titles[] = {"喝水", "取件码1234"};
  const char *requests[] = {"一分钟后提醒我喝水", "添加备忘录，取件码1234"};
  for (unsigned i = 0; i < 2; ++i) {
    reset_case(h);
    app_turn_t turn = {0};
    snprintf(turn.id, sizeof(turn.id), "positive-task-%u", i);
    char args[256];
    snprintf(args, sizeof(args),
        "{\"kind\":\"%s\",\"title\":\"%s\",\"delay_seconds\":%u,\"request\":\"%s\"}",
        kinds[i], titles[i], i == 0 ? 60 : 0, requests[i]);
    char *reply = app_agent_execute("prepare_task", args, requests[i], &turn);
    assert(reply);
    cJSON *candidate = cJSON_Parse(reply);
    free(reply);
    assert(candidate && !cJSON_HasObjectItem(candidate, "error"));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(candidate, "status")->valuestring,
                   "pending_not_executed"));
    assert(cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(candidate, "saved")));
    assert(persist_calls == 0 && !h->tasks[0] && !persisted.tasks[0]);
    assert(!app_agent_command("他说确认执行", h));
    assert(persist_calls == 0 && !persisted.tasks[0]);
    now_us += 1000000; /* A later user turn, within the confirmation window. */
    contains(app_agent_command("确认执行", h), "本机任务已保存");
    assert(persist_calls == 1 && dispatches == 0);
    assert(!strcmp(h->tasks, persisted.tasks));
    cJSON *tasks = cJSON_Parse(persisted.tasks);
    assert(cJSON_IsArray(tasks) && cJSON_GetArraySize(tasks) == 1);
    const cJSON *item = cJSON_GetArrayItem(tasks, 0);
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(item, "id")->valuestring,
                   cJSON_GetObjectItemCaseSensitive(candidate, "task_id")->valuestring));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(item, "kind")->valuestring, kinds[i]));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(item, "title")->valuestring, titles[i]));
    assert(!strcmp(cJSON_GetObjectItemCaseSensitive(item, "status")->valuestring, "pending"));
    assert(cJSON_GetObjectItemCaseSensitive(item, "due_utc")->valuedouble ==
           (i == 0 ? 1704067260.0 : 0.0));
    cJSON_Delete(tasks);
    cJSON_Delete(candidate);
    char path[] = "/tmp/vox-confirmation-XXXXXX";
    int fd = mkstemp(path); assert(fd >= 0);
    FILE *snapshot = fdopen(fd, "wb"); assert(snapshot);
    assert(fwrite(&persisted, sizeof(persisted), 1, snapshot) == 1);
    assert(fclose(snapshot) == 0);
    pid_t child = fork(); assert(child >= 0);
    if (child == 0) { execl(self_path, self_path, "--reload-snapshot", path, (char *)NULL); _exit(127); }
    int status; assert(waitpid(child, &status, 0) == child);
    assert(unlink(path) == 0);
    assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    app_agent_cancel_pending();
    *h = persisted;
    assert(!app_agent_command("", h));
    contains(app_agent_command("确认执行", h), "没有有效");
    assert(persist_calls == 1 && dispatches == 0);
    printf("PASS %s_candidate_unsaved_then_exact_next_turn_confirmation_persisted_once_and_reload_preserved\n",
           kinds[i]);
    reset_case(h);
    contains(app_agent_execute("prepare_task", args, requests[i], &turn), "pending_not_executed");
    contains(app_agent_command("取消执行", h), "已取消");
    contains(app_agent_command("确认执行", h), "没有有效");
    assert(!h->tasks[0] && persist_calls == 0 && dispatches == 0);
    printf("PASS %s_cancel_then_repeated_confirmation_does_not_save\n", kinds[i]);
  }
}

int main(int argc, char **argv) {
  app_conversation_t *h = calloc(1, sizeof(*h));
  assert(h);
  self_path = argv[0];
  if (argc == 3 && !strcmp(argv[1], "--reload-snapshot")) {
    FILE *snapshot = fopen(argv[2], "rb"); assert(snapshot);
    assert(fread(h, sizeof(*h), 1, snapshot) == 1); assert(fclose(snapshot) == 0);
    assert(!app_agent_command("", h));
    contains(app_agent_command("确认执行", h), "没有有效");
    cJSON *tasks = cJSON_Parse(h->tasks);
    assert(cJSON_IsArray(tasks) && cJSON_GetArraySize(tasks) == 1);
    assert(persist_calls == 0 && dispatches == 0);
    cJSON_Delete(tasks); free(h);
    puts("PASS fresh_process_retains_saved_task_without_confirmation_replay");
    return 0;
  }
  task_confirmation_success(h);
  if (argc == 2 && !strcmp(argv[1], "--task-confirmation-only")) {
    free(h);
    puts("PASS targeted task confirmation; real Agent C, synthetic storage, no network/device/model");
    return 0;
  }

  /* Provenance binds text only. The device path still requires a later exact
   * confirmation; these phrases alone must never dispatch. */
  const char *phrases[] = {"不要打开灯", "他说打开灯", "以后太热再打开灯"};
  for (unsigned i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
    reset_case(h);
    cJSON *args = cJSON_Parse("{\"request\":\"打开灯\"}");
    assert(app_agent_request_matches(args, phrases[i]));
    cJSON_Delete(args);
    prepare(h, "semantic-source", phrases[i]);
    assert(dispatches == 0 && persist_calls == 0);
    assert(!app_agent_command("他说确认执行", h));
    assert(dispatches == 0);
    printf("OBSERVED provenance_accepts_ambiguous_source_%u; no dispatch without exact confirmation\n", i + 1);
  }

  const char *note_sources[] = {
    "不要添加备忘录，取件码1234", "他说添加备忘录，取件码1234",
    "以后收到短信再添加备忘录，取件码1234"
  };
  for (unsigned i = 0; i < sizeof(note_sources) / sizeof(note_sources[0]); ++i) {
    reset_case(h);
    app_turn_t turn = {0};
    snprintf(turn.id, sizeof(turn.id), "note-source-%u", i);
    const char *args = "{\"kind\":\"note\",\"title\":\"取件码1234\",\"delay_seconds\":0,\"request\":\"添加备忘录，取件码1234\"}";
    char *reply = app_agent_execute("prepare_task", args, note_sources[i], &turn);
    assert(reply && dispatches == 0);
    printf("OBSERVED ambiguous_note_source_%u persist_calls=%u reply=%s\n", i + 1, persist_calls, reply);
    char name[96];
    snprintf(name, sizeof(name), "ambiguous_note_source_%u_does_not_autocommit", i + 1);
    contract(name, persist_calls == 0 && !persisted.tasks[0]);
    free(reply);
  }

  reset_case(h);
  app_turn_t alarm_turn = {0};
  snprintf(alarm_turn.id, sizeof(alarm_turn.id), "negated-alarm");
  char *alarm_reply = app_agent_execute("prepare_task",
      "{\"kind\":\"alarm\",\"title\":\"喝水\",\"delay_seconds\":60,\"request\":\"一分钟后提醒我喝水\"}",
      "不要一分钟后提醒我喝水", &alarm_turn);
  assert(alarm_reply && dispatches == 0);
  printf("OBSERVED negated_alarm persist_calls=%u reply=%s\n", persist_calls, alarm_reply);
  contract("negated_alarm_does_not_autocommit", persist_calls == 0 && !persisted.tasks[0]);
  free(alarm_reply);

  reset_case(h);
  prepare(h, "intent-failure", "打开灯");
  storage_ok = false;
  contains(app_agent_command("确认执行", h), "SD写入失败");
  assert(persist_calls == 1 && dispatches == 0 && !h->tasks[0]);
  puts("PASS failed_intent_commit_blocks_dispatch_and_rolls_back");

  reset_case(h);
  prepare(h, "ack-success", "打开灯");
  contains(app_agent_command("确认执行", h), "目标设备已确认执行");
  assert(dispatches == 1);
  printf("OBSERVED ack_success persist_calls=%u durable=%s\n", persist_calls, persisted.tasks);
  contract("ack_success_has_persistent_terminal_receipt",
           persist_calls >= 2 && strstr(persisted.tasks, "acknowledged"));
  contains(app_agent_command("确认执行", h), "没有有效");
  assert(dispatches == 1);
  puts("PASS repeated_confirmation_does_not_dispatch_twice");

  reset_case(h);
  prepare(h, "ack-storage-failure", "打开灯");
  fail_after_ack = true;
  char *reply = app_agent_command("确认执行", h);
  assert(reply && dispatches == 1);
  printf("OBSERVED storage_fails_after_ack persist_calls=%u reply=%s\n", persist_calls, reply);
  contract("ack_then_storage_failure_is_attempted_and_reported",
           persist_calls >= 2 && strstr(reply, "回执") &&
           (strstr(reply, "失败") || strstr(reply, "未保存") || strstr(reply, "未确认")));
  free(reply);

  reset_case(h);
  prepare(h, "lost-ack", "打开灯");
  lose_ack = true;
  contains(app_agent_command("确认执行", h), "结果未知");
  assert(dispatches == 1);
  contract("lost_ack_has_persistent_unknown_outcome",
           persist_calls >= 2 && strstr(persisted.tasks, "outcome_unknown"));
  contains(app_agent_command("确认执行", h), "没有有效");
  app_agent_cancel_pending();
  *h = persisted;
  assert(!app_agent_command("", h));
  contains(app_agent_command("确认执行", h), "没有有效");
  assert(dispatches == 1);
  puts("PASS unknown_action_not_replayed_after_repeat_or_context_reload (host simulation, not power-cut)");

  reset_case(h);
  prepare(h, "cancelled-action", "打开灯");
  contains(app_agent_command("取消执行", h), "已取消");
  contains(app_agent_command("确认执行", h), "没有有效");
  assert(dispatches == 0);
  puts("PASS cancelled_confirmation_does_not_dispatch");

  reset_case(h);
  prepare(h, "expired-action", "打开灯");
  now_us += 121000000;
  contains(app_agent_command("确认执行", h), "没有有效");
  assert(dispatches == 0);
  puts("PASS confirmation_expires_under_virtual_clock");

  reset_case(h);
  lose_ack = true;
  for (unsigned i = 0; i < 8; ++i) {
    char id[32];
    snprintf(id, sizeof(id), "unknown-action-%u", i);
    prepare(h, id, "打开灯");
    contains(app_agent_command("确认执行", h), "结果未知");
  }
  prepare(h, "unknown-action-8", "打开灯");
  contains(app_agent_command("确认执行", h), "未执行");
  assert(dispatches == 8);
  cJSON *records = cJSON_Parse(persisted.tasks);
  assert(cJSON_IsArray(records));
  printf("OBSERVED full_unknown_ledger dispatches=%u retained_records=%d oldest_retained=%s\n",
         dispatches, cJSON_GetArraySize(records),
         strstr(persisted.tasks, "unknown-action-0") ? "yes" : "no");
  contract("unreconciled_action_evidence_not_evicted_by_task_capacity",
           strstr(persisted.tasks, "unknown-action-0") != NULL);
  cJSON_Delete(records);
  app_agent_cancel_pending();
  free(h);
  printf("RESULT unmet_contracts=%u; synthetic input, mocked storage/device, real Agent source\n", gaps);
  /* Characterization mode is optional; default is a failing release gate. */
  return gaps && !(argc == 2 && !strcmp(argv[1], "--characterize")) ? 1 : 0;
}
