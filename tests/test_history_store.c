#define _DARWIN_C_SOURCE
#include "app_history_store.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static const char *fail_stage;
bool app_history_test_fail(const char *stage) { return fail_stage && !strcmp(stage, fail_stage); }
static char *read_file(const char *path) {
  FILE *f = fopen(path, "rb"); assert(f); assert(!fseek(f, 0, SEEK_END));
  long n = ftell(f); assert(n >= 0); rewind(f);
  char *text = calloc(1, (size_t)n + 1); assert(text);
  assert(fread(text, 1, (size_t)n, f) == (size_t)n && !fclose(f)); return text;
}
static void test_targeted(const char *dir, app_conversation_t *h, app_conversation_t *restored) {
  char path[256]; snprintf(path, sizeof(path), "%s/dialogue.jsonl", dir);
  cJSON *notes = cJSON_Parse("[{\"key\":\"饮品\",\"value\":\"茶\",\"source_turn_id\":\"removed-1\",\"revisions\":[{\"value\":\"咖啡\",\"source_turn_id\":\"removed-old\"}]}]");
  assert(notes);
  app_turn_t turn; app_turn_event_t event = {.type = TURN_USER_TEXT, .sequence = 1, .text = "无关的猫叫糯米"};
  memset(h, 0, sizeof(*h)); assert(app_history_erase(dir, h));
  app_turn_init(&turn, "keep", 1, 0); assert(app_history_event(dir, &turn, &event, 1704067200));
  char *original = read_file(path);
  assert(app_history_forget(dir, h, notes)); char *after = read_file(path); assert(!strcmp(original, after)); free(after);
  assert(app_history_forget(dir, h, notes)); after = read_file(path); assert(!strcmp(original, after)); free(after); free(original);
  app_turn_init(&turn, "removed", 1, 0); event.text = "记住饮品是茶"; assert(app_history_event(dir, &turn, &event, 1704067200));
  event.type = TURN_ASSISTANT_TEXT; event.text = "我记住了"; assert(app_history_event(dir, &turn, &event, 1704067200));
  app_turn_init(&turn, "old", 1, 0); event.text = "我喜欢咖啡"; assert(app_history_event(dir, &turn, &event, 1704067200));
  assert(app_history_forget(dir, h, notes)); after = read_file(path);
  assert(strstr(after, "糯米") && !strstr(after, "我记住了") && !strstr(after, "咖啡") && !strstr(after, "记住饮品")); free(after);
  assert(app_history_restore(dir, restored) == 1);
  const char *stages[] = {"prepare_snapshot", "write", "rename", "snapshot"};
  app_conversation_t *next = calloc(1, sizeof(*next)); assert(next);
  for (unsigned i = 0; i < 4; ++i) {
    assert(app_history_erase(dir, h)); event.type = TURN_USER_TEXT; event.text = "无关的猫叫糯米";
    app_turn_init(&turn, "keep", 1, 0); assert(app_history_event(dir, &turn, &event, 1704067200));
    memset(h, 0, sizeof(*h)); strcpy(h->notes, "[{\"key\":\"饮品\",\"value\":\"茶\",\"valid_until\":0},{\"key\":\"猫名\",\"value\":\"糯米\",\"valid_until\":0}]");
    assert(app_conversation_add_source(h, event.text, "好的，糯米", turn.id));
    app_turn_init(&turn, "removed", 1, 0); event.text = "记住饮品是茶"; assert(app_history_event(dir, &turn, &event, 1704067200));
    assert(app_conversation_add_source(h, event.text, "已保存茶", turn.id)); assert(app_history_save(dir, h));
    memcpy(next, h, sizeof(*next)); assert(app_conversation_forget(next, notes));
    original = read_file(path); fail_stage = stages[i]; assert(!app_history_forget(dir, next, notes)); fail_stage = NULL;
    char fence[256]; snprintf(fence, sizeof(fence), "%s/erase.pending", dir); assert(!access(fence, F_OK));
    assert(app_history_restore(dir, restored) == -1);
    cJSON *query = app_history_search(dir, "糯米"); assert(cJSON_GetObjectItem(query, "error")); cJSON_Delete(query);
    after = read_file(path);
    if (strcmp(stages[i], "snapshot")) assert(!strcmp(original, after));
    else assert(strstr(after, "糯米") && !strstr(after, "记住饮品"));
    free(after); free(original);
    // 模拟重启已阻断普通恢复，再从校验过的请求重试，保留无关状态。
    assert(app_history_retry_forget(dir, restored)); assert(access(fence, F_OK) != 0);
    assert(strstr(restored->notes, "糯米") && !strstr(restored->notes, "茶") && restored->count == 1);
    after = read_file(path); assert(strstr(after, "糯米") && !strstr(after, "记住饮品"));
    assert(!app_history_retry_forget(dir, restored)); original = read_file(path); assert(!strcmp(original, after)); free(original); free(after);
    assert(app_history_restore(dir, h) == 1 && !strstr(h->notes, "茶"));
  }
  free(next);
  const char *bad[] = {"{broken}\n", "{\"text\":\"无关数据\"}\n", "{\"turn_id\":\"\",\"text\":\"无关数据\"}\n",
      "{\"turn_id\":\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\",\"text\":\"无关数据\"}\n"};
  for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    assert(app_history_erase(dir, h)); FILE *f = fopen(path, "wb"); assert(f); fputs(bad[i], f); assert(!fclose(f));
    assert(!app_history_forget(dir, h, notes)); after = read_file(path); assert(!strcmp(after, bad[i])); free(after);
    assert(app_history_restore(dir, restored) == -1);
  }
  // 校验标记损坏不能从旧快照恢复，更不能重新写入一条未经确认的删除。
  assert(app_history_erase(dir, h));
  event.type = TURN_USER_TEXT; event.text = "无关的猫叫糯米"; app_turn_init(&turn, "keep", 1, 0);
  assert(app_history_event(dir, &turn, &event, 1704067200));
  char fence[256]; snprintf(fence, sizeof(fence), "%s/erase.pending", dir);
  FILE *broken = fopen(fence, "wb"); assert(broken); fputs("{truncated", broken); assert(!fclose(broken));
  original = read_file(path); assert(!app_history_retry_forget(dir, restored));
  assert(app_history_restore(dir, restored) == -1); after = read_file(path); assert(!strcmp(original, after)); free(original); free(after);
  assert(app_history_erase(dir, h));
  // 超过播放原窗口的回答分块归档，命中块带原答案偏移。
  char long_text[6000]; memset(long_text, 'x', sizeof(long_text) - 1); long_text[5999] = 0; memcpy(long_text + 3000, "长答", 6);
  event.type = TURN_ASSISTANT_TEXT; event.text = long_text; app_turn_init(&turn, "long", 1, 0);
  assert(app_history_event(dir, &turn, &event, 1704067200));
  cJSON *query = app_history_search(dir, "长答"), *match = cJSON_GetArrayItem(cJSON_GetObjectItem(query, "matches"), 0);
  assert(match && cJSON_GetObjectItem(match, "answer_total_bytes")->valueint == 5999);
  assert(cJSON_GetObjectItem(match, "answer_chunk_offset")->valueint > 0); cJSON_Delete(query);
  app_history_filter_t filter = {.from_utc = 1704067200, .to_utc = 1704067201, .role = "assistant", .turn_id = turn.id};
  query = app_history_query_filtered(dir, "长答", 0, NULL, &filter); assert(cJSON_GetArraySize(cJSON_GetObjectItem(query, "matches")) == 1); cJSON_Delete(query);
  filter.role = "user"; query = app_history_query_filtered(dir, "长答", 0, NULL, &filter); assert(!cJSON_GetArraySize(cJSON_GetObjectItem(query, "matches"))); cJSON_Delete(query);
  assert(app_history_index_tick(dir));
  FILE *f = fopen(path, "ab"); assert(f); for (unsigned i = 0; i < 40000; ++i) fputc('x', f); fputc('\n', f); fclose(f);
  event.text = "坏长行之后的原文"; assert(app_history_event(dir, &turn, &event, 1704067200));
  for (unsigned i = 0; i < 4; ++i) assert(app_history_index_tick(dir));
  query = app_history_search(dir, "坏长行"); assert(cJSON_IsTrue(cJSON_GetObjectItem(query, "indexed"))); assert(cJSON_GetArraySize(cJSON_GetObjectItem(query, "matches")) == 1); cJSON_Delete(query);
  assert(app_history_erase(dir, h)); cJSON_Delete(notes);
}

static void test_volumes(const char *dir,app_conversation_t *h) {
  char path[256],archive[256];snprintf(path,sizeof(path),"%s/dialogue.jsonl",dir);
  assert(app_history_erase(dir,h));
  app_turn_t turn;app_turn_init(&turn,"volumes",1,0);
  app_turn_event_t event={.type=TURN_USER_TEXT,.text="新卷",.sequence=1};
  for(unsigned i=1;i<=4;i++) {
    FILE *f=fopen(path,"wb");assert(f);assert(!fseek(f,8*1024*1024-128*1024,SEEK_SET));fputc('\n',f);assert(!fclose(f));
    assert(app_history_event(dir,&turn,&event,1704067200));
    unsigned size,archives;assert(app_history_capacity(dir,&size,&archives));assert(size<1024 && archives==i);
    snprintf(archive,sizeof(archive),"%s/dialogue.%u.jsonl",dir,i);assert(!access(archive,F_OK));
  }
  FILE *f=fopen(path,"wb");assert(f);assert(!fseek(f,8*1024*1024-128*1024,SEEK_SET));fputc('\n',f);fclose(f);
  assert(!app_history_event(dir,&turn,&event,1704067200));
  event.type=TURN_ASSISTANT_TEXT;assert(!app_history_event(dir,&turn,&event,1704067200));
  // 修改封存卷为有效小样本，定向删除必须覆盖封存而不仅是当前卷。
  assert(app_history_erase(dir,h));
  snprintf(archive,sizeof(archive),"%s/dialogue.1.jsonl",dir);
  f=fopen(archive,"wb");assert(f);fputs("{\"turn_id\":\"erase-1\",\"text\":\"饮品咖啡\"}\n{\"turn_id\":\"keep-1\",\"text\":\"猫叫糯米\"}\n",f);fclose(f);
  cJSON *notes=cJSON_Parse("[{\"key\":\"饮品\",\"value\":\"咖啡\",\"source_turn_id\":\"erase-1\"}]");assert(notes);
  assert(app_history_forget(dir,h,notes));char *text=read_file(archive);assert(!strstr(text,"咖啡") && strstr(text,"糯米"));free(text);
  cJSON *result=app_history_search(dir,"糯米");assert(cJSON_GetObjectItem(result,"archived_volumes_not_searched")->valueint==1);cJSON_Delete(result);
  cJSON_Delete(notes);assert(app_history_erase(dir,h));assert(access(archive,F_OK)!=0);
}

int main(void) {
  char dir[] = "/tmp/vox-history-XXXXXX";
  assert(mkdtemp(dir));
  app_conversation_t *h = calloc(1, sizeof(*h));
  app_conversation_t *restored = calloc(1, sizeof(*restored));
  assert(h && restored);
  assert(app_history_restore(dir, restored) == 0);
  assert(app_conversation_add(h, "我家猫叫糯米。\n不要改名", "好的，糯米。"));
  assert(app_history_save(dir, h));
  assert(app_history_restore(dir, restored) == 1);
  assert(restored->count == 1 && strcmp(h->turns[0].user, restored->turns[0].user) == 0);
  for (int i = 0; i < 8; ++i) {
    char text[20];
    snprintf(text, sizeof(text), "问题%d", i);
    assert(app_conversation_add(h, text, "回答"));
  }
  assert(app_history_save(dir, h));
  assert(app_history_restore(dir, restored) == 1 && restored->count == 6);
  assert(strcmp(restored->turns[0].user, "问题2") == 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/context0.json", dir);
  FILE *f = fopen(path, "wb");
  assert(f); fputs("{\"turns\":[", f); fclose(f);
  assert(app_history_restore(dir, restored) == 1 && restored->count == 1);
  // 损坏的新槽不会覆盖旧槽；接下来保存可修复新槽。
  assert(app_history_save(dir, h));
  assert(app_history_restore(dir, restored) == 1 && restored->count == 6);
  f = fopen(path, "r+b"); assert(f); fputc('[', f); fclose(f);
  assert(app_history_restore(dir, restored) == 1 && restored->count == 1);

  app_turn_t turn;
  app_turn_init(&turn, "boot", 1, 0);
  app_turn_event_t event = {.turn_id = turn.id, .sequence = 1,
      .type = TURN_USER_TEXT, .text = "猫叫\"糯米\"\n第二行", .at_us = 1};
  assert(app_history_event(dir, &turn, &event, 0));
  snprintf(path, sizeof(path), "%s/dialogue.jsonl", dir);
  f = fopen(path, "ab"); assert(f); fputs("{broken", f); fclose(f);
  event.sequence = 2; event.type = TURN_ERROR; event.text = NULL; event.error = 123;
  assert(app_history_event(dir, &turn, &event, 0));
  f = fopen(path, "rb"); assert(f);
  char line[4096]; int valid = 0, bad = 0;
  while (fgets(line, sizeof(line), f)) {
    if (line[0] == '\n') continue;
    cJSON *r = cJSON_Parse(line);
    if (r) {
      ++valid;
      if (valid == 1) assert(strcmp(cJSON_GetObjectItem(r, "text")->valuestring,
                                     "猫叫\"糯米\"\n第二行") == 0);
      cJSON_Delete(r);
    } else ++bad;
  }
  fclose(f);
  assert(valid == 2 && bad == 1);
  cJSON *matches = app_history_search(dir, "糯米");
  assert(matches && cJSON_GetArraySize(cJSON_GetObjectItem(matches, "matches")) == 1);
  cJSON_Delete(matches);
  // 活动讨论随双槽快照恢复，不执行任何待确认动作。
  assert(app_conversation_add_source(h, "比较两种供电方案", "1.充电宝。2.内置电池。", "source-1"));
  assert(app_conversation_add_source(h, "第二个但不依赖电脑", "使用内置电池。", "source-2"));
  assert(app_history_save(dir, h));
  assert(app_history_restore(dir, restored) == 1 && !strcmp(h->discussion, restored->discussion));
  assert(strstr(restored->discussion, "source-2"));
  // 长原文不再被跳过：返回命中附近片段，按ID读取完整原文。
  char long_text[1600]; memset(long_text, 'x', sizeof(long_text));
  memcpy(long_text + 1100, "供电方案", strlen("供电方案")); long_text[1599] = 0;
  event.type = TURN_ASSISTANT_TEXT; event.sequence = 3; event.text = long_text;
  assert(app_history_event(dir, &turn, &event, 1704067200));
  for (int i = 0; i < 3; ++i) assert(app_history_index_tick(dir));
  matches = app_history_search(dir, "电池");
  const cJSON *list = cJSON_GetObjectItem(matches, "matches");
  assert(cJSON_GetArraySize(list) == 1 && cJSON_IsTrue(cJSON_GetObjectItem(matches, "indexed")));
  const cJSON *match = cJSON_GetArrayItem(list, 0);
  assert(strstr(cJSON_GetObjectItem(match, "text")->valuestring, "供电方案"));
  assert(cJSON_IsTrue(cJSON_GetObjectItem(match, "truncated")));
  char record_id[32]; strcpy(record_id, cJSON_GetObjectItem(match, "record_id")->valuestring);
  cJSON_Delete(matches);
  matches = app_history_query(dir, "", 0, record_id);
  list = cJSON_GetObjectItem(matches, "matches");
  assert(cJSON_GetArraySize(list) == 1 && !strcmp(cJSON_GetObjectItem(cJSON_GetArrayItem(list, 0), "text")->valuestring, long_text));
  cJSON_Delete(matches);
  matches = app_history_query(dir, "", 0, "../../context0.json");
  assert(cJSON_GetObjectItem(matches, "error")); cJSON_Delete(matches);
  // 索引损坏不损伤原文，仍可检索并在空闲时重建。
  char index_path[256]; snprintf(index_path, sizeof(index_path), "%s/history.idx", dir);
  f = fopen(index_path, "ab"); assert(f); fputc('x', f); fclose(f);
  matches = app_history_search(dir, "电源");
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(matches, "matches")) == 1); cJSON_Delete(matches);
  assert(app_history_index_tick(dir));
  // 多次有界查询可向更早处翻页，不重返同一条。
  for (int i = 0; i < 7; ++i) {
    event.sequence++; event.text = "历史咖啡偏好";
    assert(app_history_event(dir, &turn, &event, 1704067200 + i));
  }
  assert(app_history_index_tick(dir));
  matches = app_history_search(dir, "咖啡");
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(matches, "matches")) == 3);
  long before = (long)cJSON_GetObjectItem(matches, "next_before_offset")->valuedouble;
  cJSON_Delete(matches);
  matches = app_history_query(dir, "咖啡", before, NULL);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(matches, "matches")) == 3);
  assert(cJSON_GetObjectItem(matches, "next_before_offset")->valuedouble < before); cJSON_Delete(matches);
  strcpy(h->notes, "[{\"key\":\"饮品\",\"value\":\"茶\",\"valid_until\":0}]");
  strcpy(h->tasks, "[]");
  assert(app_history_save(dir, h));
  assert(app_history_restore(dir, restored) == 1 && !strcmp(restored->notes, h->notes));
  assert(!strcmp(restored->summary, h->summary));
  // 日志容量封顶，不影响快照。
  f = fopen(path, "wb"); assert(f);
  assert(fseek(f, 8 * 1024 * 1024 - 1, SEEK_SET) == 0); fputc('\n', f); fclose(f);
  assert(!app_history_event(dir, &turn, &event, 0));
  assert(app_history_save(dir, h));
  assert(!app_history_save("/nonexistent-vox-directory", h));
  char oversized[APP_REPLY_TEXT_BYTES + 1];
  memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
  event.text = oversized;
  assert(!app_history_event(dir, &turn, &event, 0));
  unlink(path);
  for (int i = 0; i < 2; ++i) {
    snprintf(path, sizeof(path), "%s/context%d.json", dir, i);
    f = fopen(path, "wb"); assert(f); fputs("broken", f); fclose(f);
  }
  assert(app_history_restore(dir, restored) == -1 && restored->count == 0);
  snprintf(path, sizeof(path), "%s/erase.pending", dir);
  f = fopen(path, "wb"); assert(f); fputs("pending", f); fclose(f);
  assert(app_history_restore(dir, restored) == -1);
  matches = app_history_search(dir, "糯米");
  assert(cJSON_GetObjectItem(matches, "error")); cJSON_Delete(matches);
  memset(h, 0, sizeof(*h)); assert(app_history_erase(dir, h));
  assert(app_history_restore(dir, restored) == 1 && !restored->notes[0]);
  test_targeted(dir, h, restored);
  test_volumes(dir,h);
  for (int i = 0; i < 2; ++i) {
    snprintf(path, sizeof(path), "%s/context%d.json", dir, i); unlink(path);
  }
  rmdir(dir);
  free(h); free(restored);
  puts("PASS: SD snapshots/6-turn restore/UTF-8/torn write/checksum/partial journal/capacity");
}
