/*
 * 有界SSE解析：重组模型文字/工具参数，不进行网络或硬件I/O。
 * 不把未完整结束的响应当成功，不混合工具和正文分支。
 */
#include "app_sse.h"
#include <math.h>
#include <string.h>

bool app_sse_speech_ready(const app_sse_t *s, bool tools_enabled) {
  // A short preamble may be followed by tool calls. Buffer just the first
  // ~32 Chinese characters, or a completed short answer.
  return s && !s->tool_count && s->text[0] &&
      (!tools_enabled || s->stopped || strlen(s->text) >= 96);
}

static bool append(char *out, size_t cap, const cJSON *part) {
  if (!part || cJSON_IsNull(part)) return true;
  if (!cJSON_IsString(part) || strlen(out) + strlen(part->valuestring) >= cap) return false;
  strcat(out, part->valuestring);
  return true;
}
bool app_sse_event_with_json(app_sse_t *s, const char *data, cJSON **event) {
  if (!event) return false;
  *event = NULL;
  if (!s || !data || s->done) return false;
  if (strcmp(data, "[DONE]") == 0) { s->done = s->stopped; return s->done; }
  if (strstr(data, "\\u0000")) return false;
  cJSON *r = cJSON_ParseWithLengthOpts(data, strlen(data) + 1, NULL, true);
  *event = r;
  if (!r || cJSON_GetObjectItemCaseSensitive(r, "error")) return false;
  const cJSON *choices = cJSON_GetObjectItemCaseSensitive(r, "choices");
  if (cJSON_IsArray(choices) && cJSON_GetArraySize(choices) == 0) return true;
  if (s->stopped || !cJSON_IsArray(choices) || cJSON_GetArraySize(choices) != 1) return false;
  const cJSON *choice = cJSON_GetArrayItem(choices, 0);
  const cJSON *delta = cJSON_GetObjectItemCaseSensitive(choice, "delta");
  const cJSON *finish = cJSON_GetObjectItemCaseSensitive(choice, "finish_reason");
  bool ok = cJSON_IsObject(delta) && append(s->text, sizeof(s->text), cJSON_GetObjectItemCaseSensitive(delta, "content"));
  const cJSON *calls = cJSON_GetObjectItemCaseSensitive(delta, "tool_calls"), *call;
  if (calls && !cJSON_IsNull(calls) && !cJSON_IsArray(calls)) ok = false;
  if (s->speech_committed && cJSON_IsArray(calls) && cJSON_GetArraySize(calls)) {
    return false; // Late tool calls cannot mutate a device after spoken text.
  }
  cJSON_ArrayForEach(call, calls) {
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(call, "type");
    if (type && !cJSON_IsNull(type) && (!cJSON_IsString(type) || strcmp(type->valuestring, "function"))) { ok = false; break; }
    const cJSON *index = cJSON_GetObjectItemCaseSensitive(call, "index");
    const cJSON *function = cJSON_GetObjectItemCaseSensitive(call, "function");
    if (!cJSON_IsNumber(index) || !isfinite(index->valuedouble) || index->valuedouble < 0 || index->valuedouble > 3 ||
        floor(index->valuedouble) != index->valuedouble) { ok = false; break; }
    unsigned i = (unsigned)index->valuedouble;
    if (s->tool_count <= i) s->tool_count = i + 1;
    ok = ok && append(s->tools[i].id, sizeof(s->tools[i].id), cJSON_GetObjectItemCaseSensitive(call, "id")) &&
        append(s->tools[i].name, sizeof(s->tools[i].name), cJSON_GetObjectItemCaseSensitive(function, "name")) &&
        append(s->tools[i].arguments, sizeof(s->tools[i].arguments), cJSON_GetObjectItemCaseSensitive(function, "arguments"));
  }
  if (cJSON_IsString(finish)) {
    ok = ok && (strcmp(finish->valuestring, "stop") == 0 || strcmp(finish->valuestring, "tool_calls") == 0);
    if (ok) { strcpy(s->finish, finish->valuestring); s->stopped = true; }
  } else if (finish && !cJSON_IsNull(finish)) ok = false;
  return ok;
}
bool app_sse_event(app_sse_t *s, const char *data) {
  cJSON *event = NULL;
  bool ok = app_sse_event_with_json(s, data, &event);
  cJSON_Delete(event);
  return ok;
}
cJSON *app_sse_tool_message(const app_sse_t *s) {
  if (!s || !s->done || !s->tool_count || strcmp(s->finish, "tool_calls") != 0) return NULL;
  cJSON *r = cJSON_CreateObject();
  cJSON *calls = r ? cJSON_AddArrayToObject(r, "tool_calls") : NULL;
  bool ok = calls && cJSON_AddStringToObject(r, "role", "assistant") &&
      (s->text[0] ? cJSON_AddStringToObject(r, "content", s->text) : cJSON_AddNullToObject(r, "content"));
  for (unsigned i = 0; ok && i < s->tool_count; ++i) {
    if (!s->tools[i].id[0] || !s->tools[i].name[0]) { ok = false; break; }
    cJSON *c = cJSON_CreateObject();
    if (!c || !cJSON_AddItemToArray(calls, c)) { cJSON_Delete(c); ok = false; break; }
    cJSON *f = cJSON_AddObjectToObject(c, "function");
    ok = f && cJSON_AddStringToObject(c, "id", s->tools[i].id) &&
        cJSON_AddStringToObject(c, "type", "function") &&
        cJSON_AddStringToObject(f, "name", s->tools[i].name) &&
        cJSON_AddStringToObject(f, "arguments", s->tools[i].arguments);
  }
  if (!ok) { cJSON_Delete(r); return NULL; }
  return r;
}
