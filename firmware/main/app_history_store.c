/*
 * 历史文件/索引：按稳定ID、偏移、时间/角色检索有界原文片段。
 * 当前卷与封存卷分别管理，生成文本和播放终态分别记录。
 * 损坏/超长文件输入不能导致越界或无限扫描。
 */
#include "app_history_store.h"
#include "app_memory_alloc.h"
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <ctype.h>
#include <sys/stat.h>

#define SNAPSHOT_LIMIT (160 * 1024)
#define JOURNAL_LIMIT (8 * 1024 * 1024)
static unsigned generation;
static bool commit_uncertain;
static const char *journal_volumes[]={"dialogue.jsonl","dialogue.1.jsonl","dialogue.2.jsonl","dialogue.3.jsonl","dialogue.4.jsonl"};
static char blocked_turn[64],blocked_directory[256];
static bool forget_fail(const char *stage) {
#ifdef APP_HISTORY_HOST_TEST
  extern bool app_history_test_fail(const char *stage);
  return app_history_test_fail(stage);
#else
  (void)stage;
  return false;
#endif
}

static unsigned checksum(const char *s, size_t n) {
  unsigned h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ (unsigned char)s[i]) * 16777619u;
  return h;
}

static bool path_for(char *out, size_t n, const char *dir, const char *name) {
  int written = snprintf(out, n, "%s/%s", dir, name);
  return written > 0 && (size_t)written < n;
}

static bool close_synced(FILE *f, bool ok) {
  if (fflush(f) != 0) ok = false;
  if (ok && fsync(fileno(f)) != 0) ok = false;
  if (fclose(f) != 0) ok = false;
  return ok;
}

bool app_history_capacity(const char *directory,unsigned *active_bytes,unsigned *archives) {
  if(!directory || !active_bytes || !archives) return false;
  *active_bytes=*archives=0;char path[256];struct stat st;
  for(unsigned i=0;i<5;i++) {
    if(!path_for(path,sizeof(path),directory,journal_volumes[i])) return false;
    if(stat(path,&st)==0) {if(!i) *active_bytes=(unsigned)st.st_size;else ++*archives;}
    else if(errno!=ENOENT) return false;
  }
  return true;
}
static bool rotate_journal(const char *directory) {
  unsigned size,archives;
  if(!app_history_capacity(directory,&size,&archives)) return false;
  if(size<JOURNAL_LIMIT-128*1024) return true;
  if(archives>=4) return false;
  char active[256],archive[256],index[256];struct stat st;
  if(!path_for(active,sizeof(active),directory,journal_volumes[0]) || !path_for(index,sizeof(index),directory,"history.idx")) return false;
  for(unsigned i=1;i<5;i++) {
    if(!path_for(archive,sizeof(archive),directory,journal_volumes[i])) return false;
    if(stat(archive,&st)==0) continue;
    if(errno!=ENOENT) return false;
    // 索引先失效，掉电后最多重建，不会把旧索引当作新卷位置。
    if(remove(index)!=0 && errno!=ENOENT) return false;
    return rename(active,archive)==0;
  }
  return false;
}

static cJSON *read_snapshot(const char *dir, unsigned slot, unsigned *seq, bool *exists) {
  char path[256];
  if (!path_for(path, sizeof(path), dir, slot ? "context1.json" : "context0.json")) return NULL;
  FILE *f = fopen(path, "rb");
  if (!f) { if (errno != ENOENT) *exists = true; return NULL; }
  *exists = true;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long size = ftell(f);
  if (size < 12 || size > SNAPSHOT_LIMIT || fseek(f, 0, SEEK_SET) != 0) {
    fclose(f); return NULL;
  }
  char *buf = app_psram_malloc((size_t)size + 1);
  if (!buf) { fclose(f); return NULL; }
  bool ok = fread(buf, 1, (size_t)size, f) == (size_t)size;
  if (fclose(f) != 0) ok = false;
  buf[size] = 0;
  // 文件尾固定为换行、8位校验值、换行；断电半条和内容损坏不进入上下文。
  size_t json_size = (size_t)size - 10;
  char expected[11];
  snprintf(expected, sizeof(expected), "\n%08x\n", checksum(buf, json_size));
  if (!ok || memcmp(buf + json_size, expected, 10) != 0 ||
      memchr(buf, 0, json_size) || strstr(buf, "\\u0000")) { free(buf); return NULL; }
  buf[json_size] = 0;
  cJSON *root = cJSON_ParseWithLengthOpts(buf, json_size + 1, NULL, true);
  free(buf);
  const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "version");
  const cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "generation");
  const cJSON *pairs = cJSON_GetObjectItemCaseSensitive(root, "turns");
  if (!cJSON_IsNumber(v) || v->valuedouble != 1 || !cJSON_IsNumber(g) ||
      !isfinite(g->valuedouble) || g->valuedouble < 1 || g->valuedouble > 4294967294.0 ||
      floor(g->valuedouble) != g->valuedouble ||
      !cJSON_IsArray(pairs) || cJSON_GetArraySize(pairs) > APP_CONTEXT_TURNS) goto bad;
  const cJSON *p;
  cJSON_ArrayForEach(p, pairs) {
    const cJSON *u = cJSON_GetObjectItemCaseSensitive(p, "user");
    const cJSON *a = cJSON_GetObjectItemCaseSensitive(p, "assistant");
    if (!cJSON_IsString(u) || !cJSON_IsString(a) || !*u->valuestring || !*a->valuestring ||
        strlen(u->valuestring) >= APP_CONTEXT_TEXT_BYTES ||
        strlen(a->valuestring) >= APP_CONTEXT_TEXT_BYTES) goto bad;
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(p, "source_turn_id");
    if (source && (!cJSON_IsString(source) || strlen(source->valuestring) >= 64)) goto bad;
  }
  const char *keys[] = {"summary", "summary_pending", "notes", "tasks", "discussion"};
  const size_t limits[] = {2048, 12288, 8192, 4096, 3072};
  for (unsigned i = 0; i < 5; ++i) {
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(root, keys[i]);
    if (field && (!cJSON_IsString(field) || strlen(field->valuestring) >= limits[i])) goto bad;
  }
  const cJSON *discussion = cJSON_GetObjectItemCaseSensitive(root, "discussion");
  if (discussion && discussion->valuestring[0]) {
    cJSON *state = cJSON_ParseWithLengthOpts(discussion->valuestring, strlen(discussion->valuestring) + 1, NULL, true);
    bool valid = cJSON_IsObject(state);
    const cJSON *options = cJSON_GetObjectItemCaseSensitive(state, "options");
    if (options) {
      valid = valid && cJSON_IsArray(options) && cJSON_GetArraySize(options) <= 3;
      const cJSON *option;
      cJSON_ArrayForEach(option, options) valid = valid && cJSON_IsString(option) && strlen(option->valuestring) <= 320;
    }
    cJSON_Delete(state); if (!valid) goto bad;
  }
  const cJSON *notes_field = cJSON_GetObjectItemCaseSensitive(root, "notes");
  if (notes_field && notes_field->valuestring[0]) {
    cJSON *notes = cJSON_ParseWithLengthOpts(notes_field->valuestring, strlen(notes_field->valuestring) + 1, NULL, true);
    bool valid = cJSON_IsArray(notes) && cJSON_GetArraySize(notes) <= 16;
    const cJSON *note;
    cJSON_ArrayForEach(note, notes) {
      const cJSON *key = cJSON_GetObjectItemCaseSensitive(note, "key");
      const cJSON *value = cJSON_GetObjectItemCaseSensitive(note, "value");
      const cJSON *expires = cJSON_GetObjectItemCaseSensitive(note, "valid_until");
      valid = valid && cJSON_IsString(key) && *key->valuestring && strlen(key->valuestring) < 64 &&
        cJSON_IsString(value) && *value->valuestring && strlen(value->valuestring) < 256 &&
        cJSON_IsNumber(expires) && isfinite(expires->valuedouble) && expires->valuedouble >= 0 &&
        floor(expires->valuedouble) == expires->valuedouble;
      const cJSON *revisions = cJSON_GetObjectItemCaseSensitive(note, "revisions"), *revision;
      if (revisions) {
        valid = valid && cJSON_IsArray(revisions) && cJSON_GetArraySize(revisions) <= 8;
        cJSON_ArrayForEach(revision, revisions) {
          valid = valid && cJSON_IsObject(revision) && cJSON_GetArraySize(revision) <= 4;
          const cJSON *field;
          cJSON_ArrayForEach(field, revision) valid = valid && cJSON_IsString(field) && strlen(field->valuestring) < 256;
        }
      }
    }
    cJSON_Delete(notes); if (!valid) goto bad;
  }
  const cJSON *tasks_field = cJSON_GetObjectItemCaseSensitive(root, "tasks");
  if (tasks_field && tasks_field->valuestring[0]) {
    cJSON *tasks = cJSON_ParseWithLengthOpts(tasks_field->valuestring, strlen(tasks_field->valuestring) + 1, NULL, true);
    bool valid = cJSON_IsArray(tasks) && cJSON_GetArraySize(tasks) <= 8;
    const cJSON *task;
    cJSON_ArrayForEach(task, tasks) {
      const cJSON *title = cJSON_GetObjectItemCaseSensitive(task, "title");
      const cJSON *kind = cJSON_GetObjectItemCaseSensitive(task, "kind");
      const cJSON *status = cJSON_GetObjectItemCaseSensitive(task, "status");
      const cJSON *due = cJSON_GetObjectItemCaseSensitive(task, "due_utc");
      valid = valid && cJSON_IsString(title) && strlen(title->valuestring) < 128 &&
        cJSON_IsString(kind) && strlen(kind->valuestring) < 24 && cJSON_IsString(status) && strlen(status->valuestring) < 32 &&
        cJSON_IsNumber(due) && isfinite(due->valuedouble) && due->valuedouble >= 0;
    }
    cJSON_Delete(tasks); if (!valid) goto bad;
  }
  *seq = (unsigned)g->valuedouble;
  return root;
bad:
  cJSON_Delete(root);
  return NULL;
}

static void restore_fields(app_conversation_t *history, const cJSON *chosen) {
  memset(history, 0, sizeof(*history));
  if (chosen) {
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItemCaseSensitive(chosen, "turns")) {
      const cJSON *source = cJSON_GetObjectItemCaseSensitive(p, "source_turn_id");
      app_conversation_add_source(history,
          cJSON_GetObjectItemCaseSensitive(p, "user")->valuestring,
          cJSON_GetObjectItemCaseSensitive(p, "assistant")->valuestring,
          cJSON_IsString(source) ? source->valuestring : NULL);
    }
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(chosen, "summary");
    if (cJSON_IsString(field)) strcpy(history->summary, field->valuestring);
    field = cJSON_GetObjectItemCaseSensitive(chosen, "summary_pending");
    if (cJSON_IsString(field)) strcpy(history->summary_pending, field->valuestring);
    field = cJSON_GetObjectItemCaseSensitive(chosen, "notes");
    if (cJSON_IsString(field)) strcpy(history->notes, field->valuestring);
    field = cJSON_GetObjectItemCaseSensitive(chosen, "tasks");
    if (cJSON_IsString(field)) strcpy(history->tasks, field->valuestring);
    field = cJSON_GetObjectItemCaseSensitive(chosen, "discussion");
    if (cJSON_IsString(field)) strcpy(history->discussion, field->valuestring);
    field = cJSON_GetObjectItemCaseSensitive(chosen, "summary_version");
    if (cJSON_IsNumber(field) && field->valuedouble >= 0 && field->valuedouble < 4294967295.0)
      history->summary_version = (unsigned)field->valuedouble;
  }
}

int app_history_restore(const char *directory, app_conversation_t *history) {
  if (!directory || !history) return -1;
  if (app_history_deletion_pending(directory)) {
    memset(history, 0, sizeof(*history)); generation = 0;
    return -1; // 删除未完成时不恢复，明确说“重试删除”才继续持久请求。
  }
  bool exists = false;
  unsigned a = 0, b = 0;
  cJSON *one = read_snapshot(directory, 0, &a, &exists);
  cJSON *two = read_snapshot(directory, 1, &b, &exists);
  cJSON *chosen = a > b ? one : two;
  generation = a > b ? a : b;
  restore_fields(history, chosen);
  int result = chosen ? 1 : exists ? -1 : 0;
  if (result >= 0) commit_uncertain = false;
  cJSON_Delete(one); cJSON_Delete(two);
  return result;
}

cJSON *app_history_task_receipt(const char *directory, const cJSON *expected) {
  struct stat st;
  if (!directory || stat(directory, &st) != 0 || !S_ISDIR(st.st_mode) ||
      app_history_deletion_pending(directory)) return NULL;
  bool exists_a = false, exists_b = false;
  unsigned a = 0, b = 0;
  cJSON *one = read_snapshot(directory, 0, &a, &exists_a);
  cJSON *two = read_snapshot(directory, 1, &b, &exists_b);
  cJSON *tasks = NULL;
  if ((!exists_a || one) && (!exists_b || two)) {
    const cJSON *chosen = a > b ? one : two;
    const cJSON *field = cJSON_GetObjectItemCaseSensitive(chosen, "tasks");
    tasks = cJSON_IsString(field) && *field->valuestring ? cJSON_Parse(field->valuestring) : cJSON_CreateArray();
    if (!cJSON_IsArray(tasks) || cJSON_GetArraySize(tasks) > 8) { cJSON_Delete(tasks); tasks = NULL; }
    if (tasks && expected) {
      if (!cJSON_Compare(tasks, expected, true)) { cJSON_Delete(tasks); tasks = NULL; }
      else { generation = a > b ? a : b; commit_uncertain = false; }
    }
  }
  cJSON_Delete(one); cJSON_Delete(two);
  return tasks;
}

app_commit_result_t app_history_commit(const char *directory, const app_conversation_t *history) {
  if (!directory || !history || history->count > APP_CONTEXT_TURNS || generation >= 4294967294u)
    return (app_commit_result_t){APP_COMMIT_PERMANENT_FAILURE, generation, "invalid_snapshot"};
  if (commit_uncertain)
    return (app_commit_result_t){APP_COMMIT_UNCERTAIN, generation + 1, "restore_required"};
  unsigned next = generation + 1;
  cJSON *root = cJSON_CreateObject();
  cJSON *pairs = root ? cJSON_AddArrayToObject(root, "turns") : NULL;
  bool ok = pairs && cJSON_AddNumberToObject(root, "version", 1) &&
      cJSON_AddNumberToObject(root, "generation", next) &&
      cJSON_AddStringToObject(root, "summary", history->summary) &&
      cJSON_AddStringToObject(root, "summary_pending", history->summary_pending) &&
      cJSON_AddStringToObject(root, "notes", history->notes) &&
      cJSON_AddStringToObject(root, "tasks", history->tasks) &&
      cJSON_AddStringToObject(root, "discussion", history->discussion) &&
      cJSON_AddNumberToObject(root, "summary_version", history->summary_version);
  for (size_t i = 0; ok && i < history->count; ++i) {
    cJSON *p = cJSON_CreateObject();
    if (!p) { ok = false; break; }
    ok = cJSON_AddStringToObject(p, "user", history->turns[i].user) &&
         cJSON_AddStringToObject(p, "assistant", history->turns[i].assistant) &&
         cJSON_AddStringToObject(p, "source_turn_id", history->turns[i].source_turn_id);
    if (!ok || !cJSON_AddItemToArray(pairs, p)) { cJSON_Delete(p); ok = false; }
  }
  char *json = ok ? cJSON_PrintUnformatted(root) : NULL;
  cJSON_Delete(root);
  if (!json) return (app_commit_result_t){APP_COMMIT_RETRYABLE_FAILURE, generation, "serialization_failed"};
  char path[256];
  ok = strlen(json) + 10 <= SNAPSHOT_LIMIT && path_for(path, sizeof(path), directory,
      next % 2 ? "context1.json" : "context0.json");
  if (!ok) {
    free(json);
    return (app_commit_result_t){APP_COMMIT_PERMANENT_FAILURE, generation, "snapshot_too_large_or_path_invalid"};
  }
  FILE *f = fopen(path, "wb");
  bool opened = f != NULL;
  if (f) {
    ok = fprintf(f, "%s\n%08x\n", json, checksum(json, strlen(json))) > 0;
    ok = close_synced(f, ok);
  } else ok = false;
  free(json);
  if (ok) {
    unsigned read_generation = 0;
    bool exists = false;
    cJSON *verified = read_snapshot(directory, next % 2, &read_generation, &exists);
    ok = verified && read_generation == next;
    cJSON_Delete(verified);
  }
  // 只覆盖较旧槽；新槽写失败仍保留上一份完整快照。
  if (ok) generation = next;
  else if (opened) commit_uncertain = true;
  return (app_commit_result_t){ok ? APP_COMMIT_COMMITTED : opened ? APP_COMMIT_UNCERTAIN :
      APP_COMMIT_RETRYABLE_FAILURE, ok || opened ? next : generation,
      ok ? "verified" : opened ? "write_or_verification_uncertain" : "open_failed"};
}

bool app_history_save(const char *directory, const app_conversation_t *history) {
  return app_history_commit(directory, history).status == APP_COMMIT_COMMITTED;
}

static bool history_event_chunk(const char *directory, const app_turn_t *t,
                       const app_turn_event_t *e, long long utc_seconds, size_t offset, size_t total) {
  if (!directory || !t || !e) return false;
  if (e->text && strlen(e->text) >= APP_CONTEXT_TEXT_BYTES) return false;
  cJSON *r = cJSON_CreateObject();
  bool ok = r && cJSON_AddNumberToObject(r, "version", 1) &&
      cJSON_AddStringToObject(r, "profile", "local") &&
      cJSON_AddStringToObject(r, "turn_id", t->id) &&
      cJSON_AddStringToObject(r, "session_id", t->session_id) &&
      cJSON_AddNumberToObject(r, "sequence", e->sequence) &&
      cJSON_AddStringToObject(r, "event", app_turn_event_name(e->type)) &&
      cJSON_AddNumberToObject(r, "at_us", (double)e->at_us) &&
      cJSON_AddNumberToObject(r, "utc_seconds", (double)utc_seconds) &&
      cJSON_AddNumberToObject(r, "error", e->error) &&
      cJSON_AddNumberToObject(r, "generation_status", t->stages[TURN_GENERATE]) &&
      cJSON_AddNumberToObject(r, "playback_status", t->stages[TURN_PLAY]) &&
      cJSON_AddNumberToObject(r, "pcm_submitted_bytes", (double)t->pcm_submitted_bytes) &&
      cJSON_AddNumberToObject(r, "played_text_bytes", (double)t->played_text_bytes);
  if (ok && e->text) ok = cJSON_AddStringToObject(r, "text", e->text) != NULL;
  if (ok && total >= APP_CONTEXT_TEXT_BYTES) ok =
      cJSON_AddNumberToObject(r, "text_offset", (double)offset) &&
      cJSON_AddNumberToObject(r, "text_total_bytes", (double)total);
  char *json = ok ? cJSON_PrintUnformatted(r) : NULL;
  cJSON_Delete(r);
  if (!json) return false;
  char path[256];
  FILE *f = path_for(path, sizeof(path), directory, "dialogue.jsonl") ? fopen(path, "ab+") : NULL;
  ok = false;
  if (f) {
    if (fseek(f, 0, SEEK_END) == 0) {
      long size = ftell(f);
      if (size >= 0 && (size_t)size + strlen(json) + 2 <= JOURNAL_LIMIT)
        // 前置换行隔开上次断电留下的半条，离线读取忽略空行和无效JSON行。
        ok = fprintf(f, "\n%s\n", json) > 0;
    }
    ok = close_synced(f, ok);
  }
  free(json);
  return ok;
}

bool app_history_event(const char *directory, const app_turn_t *t,
                       const app_turn_event_t *e, long long utc_seconds) {
  if (!directory || !t || !e || app_history_deletion_pending(directory)) return false;
  if(e->type==TURN_USER_TEXT) {
    blocked_turn[0]=blocked_directory[0]=0;
    // 一轮开始前分卷，留出整个长回答及其终态空间，不拆开同一轮。
    if(!rotate_journal(directory)) {
      snprintf(blocked_turn,sizeof(blocked_turn),"%s",t->id);
      snprintf(blocked_directory,sizeof(blocked_directory),"%s",directory);return false;
    }
  }
  if(!strcmp(blocked_turn,t->id) && !strcmp(blocked_directory,directory)) return false;
  size_t total = e->text ? strlen(e->text) : 0;
  if (total >= APP_REPLY_TEXT_BYTES) return false;
  if (total < APP_CONTEXT_TEXT_BYTES) return history_event_chunk(directory, t, e, utc_seconds, 0, total);
  for (size_t offset = 0; offset < total;) {
    size_t n = total - offset;
    if (n >= APP_CONTEXT_TEXT_BYTES) n = APP_CONTEXT_TEXT_BYTES - 1;
    while (n && ((unsigned char)e->text[offset + n] & 0xc0) == 0x80) --n;
    if (!n) return false;
    char *part = malloc(n + 1);
    if (!part) return false;
    memcpy(part, e->text + offset, n); part[n] = 0;
    app_turn_event_t chunk = *e; chunk.text = part;
    bool ok = history_event_chunk(directory, t, &chunk, utc_seconds, offset, total);
    free(part); if (!ok) return false;
    offset += n;
  }
  return true;
}

// 固定32字节定位记录；校验失败只重建索引，不动原文。
typedef struct {
  uint32_t magic, offset, next, length, text_crc, utc_low, utc_high, crc;
} history_index_t;
#define INDEX_MAGIC 0x56495831u
#define INDEX_SKIP_LINE UINT32_MAX
_Static_assert(sizeof(history_index_t) == 32, "fixed history index layout");

static bool deletion_fenced(const char *directory) {
  char path[256];
  if (!path_for(path, sizeof(path), directory, "erase.pending")) return true;
  FILE *f = fopen(path, "rb");
  if (f) { fclose(f); return true; }
  return errno != ENOENT;
}
bool app_history_deletion_pending(const char *directory) {
  return !directory || deletion_fenced(directory);
}
static bool index_read(FILE *f, long slot, history_index_t *entry, long journal_size) {
  return slot >= 0 && fseek(f, slot * (long)sizeof(*entry), SEEK_SET) == 0 &&
      fread(entry, sizeof(*entry), 1, f) == 1 && entry->magic == INDEX_MAGIC &&
      entry->crc == checksum((const char *)entry, sizeof(*entry) - sizeof(entry->crc)) &&
      entry->offset < entry->next && entry->next <= (unsigned long)journal_size &&
      (!entry->length || entry->length == INDEX_SKIP_LINE || entry->length == entry->next - entry->offset);
}

bool app_history_index_tick(const char *directory) {
  if (!directory || deletion_fenced(directory)) return false;
  char path[256];
  FILE *journal = path_for(path, sizeof(path), directory, "dialogue.jsonl") ? fopen(path, "rb") : NULL;
  if (!journal) return false;
  if (fseek(journal, 0, SEEK_END) != 0) { fclose(journal); return false; }
  long end = ftell(journal);
  FILE *index = path_for(path, sizeof(path), directory, "history.idx") ? fopen(path, "ab+") : NULL;
  if (!index || end < 0 || end > JOURNAL_LIMIT) { if (index) fclose(index); fclose(journal); return false; }
  long start = 0;
  bool valid = fseek(index, 0, SEEK_END) == 0;
  long size = valid ? ftell(index) : -1;
  history_index_t last;
  valid = size >= 0 && size % sizeof(last) == 0;
  if (valid && size) { valid = index_read(index, size / sizeof(last) - 1, &last, end); if (valid) start = last.next; }
  if (!valid) {
    fclose(index); index = fopen(path, "wb+"); start = 0;
    if (!index) { fclose(journal); return false; }
  }
  if (start == end) { fclose(index); fclose(journal); return true; }
  bool skipping = size > 0 && valid && last.length == INDEX_SKIP_LINE;
  char *line = app_psram_malloc(16385);
  bool ok = line && fseek(journal, start, SEEK_SET) == 0 && fseek(index, 0, SEEK_END) == 0;
  long position = start;
  while (ok && position < end && position - start < 32768) {
    long offset = position;
    if (!fgets(line, 16385, journal)) break;
    position = ftell(journal);
    bool complete = strchr(line, '\n') != NULL;
    if (!complete && !skipping && position == end && strlen(line) < 16384) break;
    // 超长坏行分块跳过并记游标，不能永久卡在同一个16KiB块。
    cJSON *root = complete && !skipping ? cJSON_Parse(line) : NULL;
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(root, "text");
    const cJSON *utc = cJSON_GetObjectItemCaseSensitive(root, "utc_seconds");
    uint64_t timestamp = cJSON_IsNumber(utc) && isfinite(utc->valuedouble) && utc->valuedouble >= 0 &&
        utc->valuedouble < 1e12 ? (uint64_t)utc->valuedouble : 0;
    history_index_t entry = {.magic = INDEX_MAGIC, .offset = (uint32_t)offset, .next = (uint32_t)position,
        .utc_low = (uint32_t)timestamp, .utc_high = (uint32_t)(timestamp >> 32)};
    if (!complete) entry.length = INDEX_SKIP_LINE;
    if (cJSON_IsString(text)) {
      entry.length = (uint32_t)(position - offset);
      entry.text_crc = checksum(line, strlen(line));
    }
    cJSON_Delete(root);
    skipping = !complete;
    entry.crc = checksum((const char *)&entry, sizeof(entry) - sizeof(entry.crc));
    ok = fwrite(&entry, sizeof(entry), 1, index) == 1;
  }
  if (ferror(journal)) ok = false;
  free(line); fclose(journal);
  return close_synced(index, ok);
}

static const char *find_text(const char *text, const char *keyword) {
  size_t n = strlen(keyword);
  size_t length = strlen(text);
  for (size_t offset = 0; offset + n <= length; ++offset) {
    const char *p = text + offset;
    size_t i = 0;
    while (i < n && tolower((unsigned char)p[i]) == tolower((unsigned char)keyword[i])) ++i;
    if (i == n) return p;
  }
  // 按有限中文别名匹配，返回原文中的实际位置。
  const char *power[] = {"供电", "电池", "电源"};
  for (unsigned i = 0; i < 3; ++i) if (!strcmp(keyword, power[i])) {
    for (unsigned j = 0; j < 3; ++j) { const char *p = strstr(text, power[j]); if (p) return p; }
  }
  return NULL;
}
static bool add_history_match(cJSON *matches, const char *line, long offset,
                              const char *keyword, bool full, const app_history_filter_t *filter) {
  cJSON *r = cJSON_ParseWithLengthOpts(line, strlen(line) + 1, NULL, true);
  const cJSON *text = cJSON_GetObjectItemCaseSensitive(r, "text");
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "turn_id");
  const cJSON *event = cJSON_GetObjectItemCaseSensitive(r, "event");
  if (!cJSON_IsString(text) || !cJSON_IsString(id) || !cJSON_IsString(event) ||
      strlen(id->valuestring) >= 64 || strlen(event->valuestring) >= 64 || strlen(text->valuestring) >= APP_CONTEXT_TEXT_BYTES) {
    cJSON_Delete(r); return false;
  }
  const cJSON *utc = cJSON_GetObjectItemCaseSensitive(r, "utc_seconds");
  if (filter && ((filter->turn_id && strcmp(filter->turn_id, id->valuestring)) ||
      (filter->role && strcmp(event->valuestring, !strcmp(filter->role, "user") ? "用户文本" : "助手文本")) ||
      ((filter->from_utc || filter->to_utc) && (!cJSON_IsNumber(utc) ||
       utc->valuedouble < filter->from_utc || (filter->to_utc && utc->valuedouble >= filter->to_utc))))) {
    cJSON_Delete(r); return false;
  }
  const char *found = full ? text->valuestring : find_text(text->valuestring, keyword);
  if (!found) { cJSON_Delete(r); return false; }
  size_t start = !full && (size_t)(found - text->valuestring) > 180 ? (size_t)(found - text->valuestring) - 180 : 0;
  while (start && ((unsigned char)text->valuestring[start] & 0xc0) == 0x80) --start;
  size_t n = strlen(text->valuestring + start);
  if (!full && n > 768) n = 768;
  while (n && ((unsigned char)text->valuestring[start + n] & 0xc0) == 0x80) --n;
  char *snippet = malloc(n + 1);
  if (!snippet) { cJSON_Delete(r); return false; }
  memcpy(snippet, text->valuestring + start, n); snippet[n] = 0;
  char record[48]; snprintf(record, sizeof(record), "h-%08x-%08x", (unsigned)offset, checksum(line, strlen(line)));
  cJSON *item = cJSON_CreateObject();
  bool ok = item && cJSON_AddStringToObject(item, "record_id", record) &&
      cJSON_AddStringToObject(item, "turn_id", id->valuestring) && cJSON_AddStringToObject(item, "event", event->valuestring) &&
      cJSON_AddStringToObject(item, "text", snippet) && cJSON_AddNumberToObject(item, "text_offset", (double)start) &&
      cJSON_AddNumberToObject(item, "text_total_bytes", (double)strlen(text->valuestring)) &&
      cJSON_AddBoolToObject(item, "truncated", start > 0 || n < strlen(text->valuestring));
  const cJSON *chunk_offset = cJSON_GetObjectItemCaseSensitive(r, "text_offset");
  const cJSON *chunk_total = cJSON_GetObjectItemCaseSensitive(r, "text_total_bytes");
  if (ok && cJSON_IsNumber(chunk_offset) && cJSON_IsNumber(chunk_total)) ok =
      cJSON_AddNumberToObject(item, "answer_chunk_offset", chunk_offset->valuedouble) &&
      cJSON_AddNumberToObject(item, "answer_total_bytes", chunk_total->valuedouble);
  if (ok && cJSON_IsNumber(utc)) ok = cJSON_AddNumberToObject(item, "utc_seconds", utc->valuedouble) != NULL;
  if (ok) ok = cJSON_AddItemToArray(matches, item);
  if (!ok) cJSON_Delete(item);
  free(snippet); cJSON_Delete(r); return ok;
}

static cJSON *history_query_impl(const char *directory, const char *keyword, long before, const char *record_id,
                                  const app_history_filter_t *filter, bool browse) {
  if (!directory || before < 0 || before > JOURNAL_LIMIT ||
      (!record_id && (!keyword || (!browse && strlen(keyword) < 2) || strlen(keyword) > 48))) return NULL;
  cJSON *out = cJSON_CreateObject();
  cJSON *matches = out ? cJSON_AddArrayToObject(out, "matches") : NULL;
  if (!matches) { cJSON_Delete(out); return NULL; }
  cJSON_AddStringToObject(out, "scope", "bounded_index_or_512KiB_window_max_3_matches");
  unsigned active_bytes=0,archives=0;
  if(app_history_capacity(directory,&active_bytes,&archives)) cJSON_AddNumberToObject(out,"archived_volumes_not_searched",archives);
  cJSON_AddStringToObject(out,"volume_scope","active_dialogue.jsonl_only; archived_volumes_require_offline_export_search");
  cJSON_AddStringToObject(out, "playback", "generated_text_not_proof_of_heard_audio");
  if (deletion_fenced(directory)) { cJSON_AddStringToObject(out, "error", "deletion_incomplete"); return out; }
  char path[256];
  FILE *journal = path_for(path, sizeof(path), directory, "dialogue.jsonl") ? fopen(path, "rb") : NULL;
  char *line = journal ? app_psram_malloc(16385) : NULL;
  if (!journal || !line || fseek(journal, 0, SEEK_END) != 0) {
    if (journal) fclose(journal);
    free(line); cJSON_AddStringToObject(out, "error", "history_unavailable"); return out;
  }
  long end = ftell(journal), cursor = 0;
  bool more = false;
  if (end < 0 || end > JOURNAL_LIMIT) { cJSON_AddStringToObject(out, "error", "history_size_invalid"); goto done; }
  if (record_id) {
    unsigned offset, crc; int consumed = 0;
    bool valid = strlen(record_id) == 19 && sscanf(record_id, "h-%8x-%8x%n", &offset, &crc, &consumed) == 2 && consumed == 19 &&
        offset < (unsigned long)end && fseek(journal, offset, SEEK_SET) == 0 && fgets(line, 16385, journal) &&
        strchr(line, '\n') && checksum(line, strlen(line)) == crc && add_history_match(matches, line, offset, "", true, filter);
    if (!valid) cJSON_AddStringToObject(out, "error", "record_missing_or_changed");
    goto done;
  }
  if (!before || before > end) before = end;
  FILE *index = path_for(path, sizeof(path), directory, "history.idx") ? fopen(path, "rb") : NULL;
  bool indexed = false;
  bool corrupt = false;
  if (index && fseek(index, 0, SEEK_END) == 0) {
    long size = ftell(index), count = size > 0 ? size / sizeof(history_index_t) : 0;
    history_index_t entry;
    bool valid_index = size >= 0 && size % sizeof(entry) == 0 && (!size || index_read(index, count - 1, &entry, end));
    corrupt = !valid_index;
    indexed = valid_index && size > 0 && entry.next == (unsigned long)end;
    if (indexed) {
      long low = 0, high = count;
      while (low < high) {
        long mid = low + (high - low) / 2;
        if (!index_read(index, mid, &entry, end)) { indexed = false; corrupt = true; break; }
        if (entry.offset < (unsigned long)before) low = mid + 1; else high = mid;
      }
      size_t bytes = 0; unsigned rows = 0;
      while (indexed && low > 0 && rows++ < 2048 && bytes < 512 * 1024 && cJSON_GetArraySize(matches) < 3) {
        --low;
        if (!index_read(index, low, &entry, end)) { indexed = false; corrupt = true; break; }
        cursor = entry.offset;
        if (!entry.length || entry.length == INDEX_SKIP_LINE) continue;
        if (entry.length > 16384 || fseek(journal, entry.offset, SEEK_SET) != 0 ||
            fread(line, 1, entry.length, journal) != entry.length) { indexed = false; corrupt = true; break; }
        line[entry.length] = 0; bytes += entry.length;
        if (checksum(line, entry.length) != entry.text_crc) { indexed = false; corrupt = true; break; }
        add_history_match(matches, line, entry.offset, keyword, false, filter);
      }
      more = low > 0;
    }
  }
  if (index) fclose(index);
  if (corrupt) (void)remove(path); // 只删派生索引，空闲作业重新建立。
  if (!indexed) {
    // 索引未建完或损坏仍可有界检索；已返回的坏索引结果不混入本次材料。
    while (cJSON_GetArraySize(matches)) cJSON_DeleteItemFromArray(matches, 0);
    long start = before > 512 * 1024 ? before - 512 * 1024 : 0;
    if (fseek(journal, start, SEEK_SET) != 0) goto done;
    if (start) { int c; while ((c = fgetc(journal)) != '\n' && c != EOF) {} }
    cursor = start; more = start > 0;
    while (ftell(journal) < before) {
      long offset = ftell(journal);
      if (!fgets(line, 16385, journal)) break;
      if (!strchr(line, '\n')) { int c; while ((c = fgetc(journal)) != '\n' && c != EOF) {} continue; }
      if (ftell(journal) > before) break;
      if (add_history_match(matches, line, offset, keyword, false, filter) && cJSON_GetArraySize(matches) > 3) cJSON_DeleteItemFromArray(matches, 0);
    }
    if (cJSON_GetArraySize(matches) == 3) {
      const cJSON *id = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(matches, 0), "record_id");
      unsigned offset;
      if (cJSON_IsString(id) && sscanf(id->valuestring, "h-%8x-", &offset) == 1 && offset > 0) { cursor = offset; more = true; }
    }
  }
  cJSON_AddBoolToObject(out, "indexed", indexed);
  cJSON_AddBoolToObject(out, "more", more);
  if (more && cursor > 0) cJSON_AddNumberToObject(out, "next_before_offset", cursor);
done:
  if (ferror(journal)) cJSON_AddStringToObject(out, "error", "history_read_failed");
  free(line); fclose(journal); return out;
}
cJSON *app_history_query(const char *directory, const char *keyword, long before, const char *record_id) {
  return app_history_query_filtered(directory, keyword, before, record_id, NULL);
}
cJSON *app_history_search(const char *directory, const char *keyword) {
  return app_history_query(directory, keyword, 0, NULL);
}

bool app_history_erase(const char *directory, const app_conversation_t *replacement) {
  char path[256];
  if (!directory || !replacement || !path_for(path, sizeof(path), directory, "erase.pending")) return false;
  FILE *f = fopen(path, "wb");
  if (!f) return false;
  if (!close_synced(f, fputs("deletion-in-progress\n", f) >= 0)) return false;
  const char *files[] = {"context0.json", "context1.json", "dialogue.jsonl", "history.idx", "dialogue.rewrite", "dialogue.1.jsonl", "dialogue.2.jsonl", "dialogue.3.jsonl", "dialogue.4.jsonl"};
  for (unsigned i = 0; i < sizeof(files)/sizeof(files[0]); ++i) {
    if (!path_for(path, sizeof(path), directory, files[i])) return false;
    if (remove(path) != 0 && errno != ENOENT) return false;
  }
  generation = 0;
  if (!app_history_save(directory, replacement)) return false;
  if (!path_for(path, sizeof(path), directory, "erase.pending")) return false;
  return remove(path) == 0;
}

static bool history_forget(const char *directory, const app_conversation_t *replacement, const cJSON *notes, bool retry) {
  if (!directory || !replacement || !cJSON_IsArray(notes) || !cJSON_GetArraySize(notes)) return false;
  if (!retry && app_history_deletion_pending(directory)) return false;
  char fence[256], path[256], temporary[256];
  if (!path_for(fence, sizeof(fence), directory, "erase.pending") ||
      !path_for(path, sizeof(path), directory, "dialogue.jsonl") ||
      !path_for(temporary, sizeof(temporary), directory, "dialogue.rewrite")) return false;
  if (!retry) {
    cJSON *request = cJSON_CreateObject(), *copy = cJSON_Duplicate(notes, true);
    bool prepared = request && copy && cJSON_AddStringToObject(request, "mode", "targeted") && cJSON_AddItemToObject(request, "notes", copy);
    if (!prepared) cJSON_Delete(copy);
    char *json = prepared ? cJSON_PrintUnformatted(request) : NULL; cJSON_Delete(request);
    if (!json || strlen(json) > 8192) { free(json); return false; }
    FILE *marker = fopen(fence, "wb");
    bool marked = marker && fprintf(marker, "%s\n%08x\n", json, checksum(json, strlen(json))) > 0;
    if (marker) marked = close_synced(marker, marked);
    free(json);
    if (!marked) return false;
  }
  // 先保存已过滤的恢复副本；即使原文替换之后失败，重试也有可靠状态来源。
  if (forget_fail("prepare_snapshot") || !app_history_save(directory, replacement)) return false;
  unsigned prepared_generation = generation;
  // 对全部受管卷执行同一删除规则；失败保留屏障，重试可重复处理已完成的卷。
  for(unsigned volume=0;volume<5;volume++) {
  if(!path_for(path,sizeof(path),directory,journal_volumes[volume])) return false;
  struct stat st;
  if(volume && stat(path,&st)!=0) {if(errno==ENOENT) continue;return false;}
  FILE *input = fopen(path, "rb");
  bool ok = input || errno == ENOENT;
  FILE *output = ok ? fopen(temporary, "wb") : NULL;
  // 一轮最大16KiB生成原文，JSON最坏转义加事件元数据仍有硬上限。
  char *batch = app_psram_malloc(128 * 1024), *line = app_psram_malloc(16385);
  size_t used = 0; char current[64] = {0}; bool affected = false;
  ok = ok && output && batch && line;
  long scanned = 0;
  while (ok && input && fgets(line, 16385, input)) {
    scanned = ftell(input);
    if (scanned < 0 || scanned > JOURNAL_LIMIT) { ok = false; break; }
    if (!strchr(line, '\n')) { ok = false; break; } // 损坏长行/半条不冒险改写，屏障保留。
    if (strspn(line, " \t\r\n") == strlen(line)) {
      size_t n = strlen(line);
      if (used + n > 128 * 1024) { ok = false; break; }
      memcpy(batch + used, line, n); used += n; continue;
    }
    cJSON *record = cJSON_ParseWithLengthOpts(line, strlen(line) + 1, NULL, true);
    const cJSON *source = cJSON_GetObjectItemCaseSensitive(record, "turn_id");
    const cJSON *text = cJSON_GetObjectItemCaseSensitive(record, "text");
    if (!cJSON_IsString(source) || !*source->valuestring || strlen(source->valuestring) >= sizeof(current)) { cJSON_Delete(record); ok = false; break; }
    if (strcmp(current, source->valuestring)) {
      if (used && !affected) ok = fwrite(batch, 1, used, output) == used;
      used = 0; affected = false; strcpy(current, source->valuestring);
    }
    const cJSON *note;
    cJSON_ArrayForEach(note, notes) affected |= app_conversation_memory_matches(note,
        cJSON_IsString(text) ? text->valuestring : "", source->valuestring);
    size_t n = strlen(line);
    if (used + n > 128 * 1024) ok = false;
    else { memcpy(batch + used, line, n); used += n; }
    cJSON_Delete(record);
  }
  if (input) { if (ferror(input)) ok = false; if (fclose(input)) ok = false; }
  if (ok && used && !affected) ok = fwrite(batch, 1, used, output) == used;
  if (forget_fail("write")) ok = false;
  if (output) ok = close_synced(output, ok);
  free(batch); free(line);
  // 临时文件完整落盘后才替换原文；任何失败都不移除恢复屏障。
  if (!ok || forget_fail("rename") || rename(temporary, path) != 0) return false;
  }
  // 测试故障点表示替换后快照收尾未完成；准备好的副本仍保留在屏障内。
  if (forget_fail("snapshot")) return false;
  const char *derived[] = {"history.idx", "context0.json", "context1.json"};
  for (unsigned i = 0; i < 3; ++i) {
    if (i == 1 + prepared_generation % 2) continue; // 不删除刚验证的新槽。
    if (!path_for(path, sizeof(path), directory, derived[i]) || (remove(path) != 0 && errno != ENOENT)) return false;
  }
  return remove(fence) == 0;
}

bool app_history_forget(const char *directory, const app_conversation_t *replacement, const cJSON *notes) {
  return history_forget(directory, replacement, notes, false);
}

bool app_history_retry_forget(const char *directory, app_conversation_t *history) {
  if (!directory || !history) return false;
  char path[256]; if (!path_for(path, sizeof(path), directory, "erase.pending")) return false;
  FILE *f = fopen(path, "rb"); if (!f) return false;
  bool ok = fseek(f, 0, SEEK_END) == 0; long size = ok ? ftell(f) : -1;
  char *text = size >= 12 && size <= 8202 ? app_psram_malloc((size_t)size + 1) : NULL;
  ok = text && fseek(f, 0, SEEK_SET) == 0 && fread(text, 1, (size_t)size, f) == (size_t)size;
  if (fclose(f)) ok = false;
  cJSON *request = NULL;
  if (ok) {
    text[size] = 0; size_t n = (size_t)size - 10;
    char expected[11]; snprintf(expected, sizeof(expected), "\n%08x\n", checksum(text, n));
    ok = !memchr(text, 0, n) && !memcmp(text + n, expected, 10);
    text[n] = 0;
    if (ok) request = cJSON_ParseWithLengthOpts(text, n + 1, NULL, true);
  }
  free(text);
  const cJSON *mode = cJSON_GetObjectItemCaseSensitive(request, "mode"), *notes = cJSON_GetObjectItemCaseSensitive(request, "notes");
  ok = cJSON_IsString(mode) && !strcmp(mode->valuestring, "targeted") && cJSON_IsArray(notes) && cJSON_GetArraySize(notes) > 0 && cJSON_GetArraySize(notes) <= 16;
  const cJSON *note;
  cJSON_ArrayForEach(note, notes) {
    const cJSON *key = cJSON_GetObjectItemCaseSensitive(note, "key");
    ok = ok && cJSON_IsString(key) && *key->valuestring && strlen(key->valuestring) < 64;
  }
  bool exists = false; unsigned a = 0, b = 0;
  cJSON *one = ok ? read_snapshot(directory, 0, &a, &exists) : NULL;
  cJSON *two = ok ? read_snapshot(directory, 1, &b, &exists) : NULL;
  cJSON *chosen = a > b ? one : two;
  app_conversation_t *next = ok ? app_psram_zero(sizeof(*next)) : NULL;
  if (next && (!exists || chosen)) {
    restore_fields(next, chosen); generation = a > b ? a : b;
    ok = app_conversation_forget(next, notes) && history_forget(directory, next, notes, true);
    if (ok) memcpy(history, next, sizeof(*history));
  } else ok = false;
  free(next); cJSON_Delete(one); cJSON_Delete(two); cJSON_Delete(request); return ok;
}

cJSON *app_history_query_filtered(const char *directory,const char *keyword,long before,const char *record_id,const app_history_filter_t *filter){
  return history_query_impl(directory,keyword,before,record_id,filter,false);
}
cJSON *app_history_recent(const char *directory,long before){
  return history_query_impl(directory,"",before,NULL,NULL,true);
}
