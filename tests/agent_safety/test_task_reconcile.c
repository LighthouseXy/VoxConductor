#include <unistd.h>
#include <sys/wait.h>
#define app_control_voice_command baseline_control_command
#define app_control_voice_pending baseline_control_pending
#define app_control_voice_cancel baseline_control_cancel
#define main baseline_main
#define app_storage_persist_context baseline_persist
#define app_storage_task_receipt baseline_receipt
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#include BASELINE_TEST_PATH
#pragma GCC diagnostic pop
#undef main
#undef app_control_voice_command
#undef app_control_voice_pending
#undef app_control_voice_cancel
#undef app_storage_persist_context
#undef app_storage_task_receipt
static bool local_pending;
static unsigned local_command_calls;
bool app_control_voice_pending(void) { return local_pending; }
void app_control_voice_cancel(void) { local_pending = false; }
char *app_control_voice_command(const char *text) {
  ++local_command_calls;
  // Equivalent consuming branch from app_control.c, unlike the always-empty fixture.
  if (local_pending && !strcmp(text, "取消执行")) {
    app_control_voice_cancel(); return strdup("已取消待确认的本机操作。");
  }
  return NULL;
}
static time_t wall_now = 1800000000;
time_t time(time_t *out) { if(out) *out=wall_now; return wall_now; }
bool app_history_test_fail(const char *stage) { (void)stage; return false; }
static const char *directory;
static int save_mode;
static unsigned attempts;
bool app_storage_persist_context(const app_conversation_t *h) {
  ++attempts;
  if (save_mode == 1) return false;
  bool saved = app_history_save(directory, h);
  return saved && save_mode != 2;
}
cJSON *app_storage_task_receipt(const cJSON *expected) { return app_history_task_receipt(directory, expected); }
static void prepare(app_conversation_t *h, const char *kind) {
  free(app_agent_command("", h));
  app_turn_t turn = {0}; snprintf(turn.id, sizeof(turn.id), "reconcile-%u", attempts);
  char args[256];
  snprintf(args, sizeof(args), "{\"kind\":\"%s\",\"title\":\"synthetic\",\"delay_seconds\":%d,\"request\":\"synthetic\"}", kind, !strcmp(kind,"alarm") ? 60 : 0);
  contains(app_agent_execute("prepare_task", args, "synthetic", &turn), "pending_not_executed");
}
int main(int argc, char **argv) {
  assert(argc >= 2); directory=argv[1];
  app_conversation_t *h=calloc(1,sizeof(*h));assert(h);
  if (argc == 3) {
    assert(app_history_restore(directory,h)==1);
    contains(app_agent_command("查看任务保存状态",h),"已持久化");
    contains(app_agent_command("重试任务保存",h),"没有可重试");
    contains(app_agent_command("确认执行",h),"没有有效");
    assert(attempts==0);free(h);puts("PASS fresh process reads durable task without replay authority");return 0;
  }
  const char *kinds[]={"note","alarm"};
  for(unsigned i=0;i<2;++i) {
    app_agent_cancel_pending();memset(h,0,sizeof(*h));assert(app_history_erase(directory,h));
    attempts=0;save_mode=1;prepare(h,kinds[i]);
    contains(app_agent_command("查看任务保存状态",h),"候选");
    contains(app_agent_command("确认执行",h),"SD写入失败");assert(attempts==1);
    contains(app_agent_command("查看任务保存状态",h),"失败");
    contains(app_agent_command("重试任务保存",h),"候选");assert(attempts==1);
    save_mode=0;contains(app_agent_command("确认执行",h),"本机任务已保存");assert(attempts==2);
    contains(app_agent_command("查看任务保存状态",h),"已持久化");
    contains(app_agent_command("重试任务保存",h),"已持久化");
    contains(app_agent_command("确认执行",h),"没有有效");assert(attempts==2);
    pid_t child=fork();assert(child>=0);
    if(!child){execl(argv[0],argv[0],directory,"reload",(char*)NULL);_exit(127);}
    int status;assert(waitpid(child,&status,0)==child && WIFEXITED(status) && WEXITSTATUS(status)==0);
    app_agent_cancel_pending();memset(h,0,sizeof(*h));assert(app_history_erase(directory,h));
    attempts=0;save_mode=2;prepare(h,kinds[i]);
    contains(app_agent_command("确认执行",h),"SD写入失败");
    app_task_view_t rows[8];
    assert(app_agent_task_snapshot(rows)==0); // The failed acknowledgement rolled RAM back.
    contains(app_agent_command("重试任务保存",h),"已持久化");assert(attempts==1);
    assert(app_agent_task_snapshot(rows)==1);
    cJSON *disk = app_history_task_receipt(directory,NULL);
    cJSON *ram = cJSON_Parse(h->tasks); assert(cJSON_Compare(disk,ram,true));
    cJSON_Delete(disk); cJSON_Delete(ram);
    if (!strcmp(kinds[i],"alarm")) {
      wall_now += 61; save_mode=0;
      app_task_notice_t notice; assert(app_agent_due_notice(&notice));
      assert(!app_agent_due_notice(&notice));
      puts("PASS reconciled alarm visible to due processing exactly once");
    }
    app_agent_cancel_pending();memset(h,0,sizeof(*h));assert(app_history_erase(directory,h));
    attempts=0;save_mode=1;prepare(h,kinds[i]);contains(app_agent_command("确认执行",h),"SD写入失败");
    local_pending=true; unsigned calls=local_command_calls;
    contains(app_agent_command("取消执行",h),"已取消");
    assert(!local_pending && local_command_calls==calls);
    contains(app_agent_command("重试任务保存",h),"没有可重试");assert(attempts==1);
    contains(app_agent_command("确认执行",h),"没有有效");assert(attempts==1);
    puts("PASS local pending cancellation clears failed task retry authority before control consumes it");
    printf("PASS %s failed retry requires confirmation, durable receipt deduplicates, cancellation revokes retry\n",kinds[i]);
  }
  char path[512];snprintf(path,sizeof(path),"%s/context1.json",directory);FILE*f=fopen(path,"wb");assert(f);fputs("torn",f);fclose(f);
  assert(!app_history_task_receipt(directory,NULL));
  cJSON *expected = cJSON_CreateArray(); assert(expected);
  assert(!app_history_task_receipt(directory,expected)); cJSON_Delete(expected);
  contains(app_agent_command("查看任务保存状态",h),"校验失败");
  free(h);puts("PASS corrupt slot blocks reconciliation; real snapshot I/O, no network or device");return 0;
}
