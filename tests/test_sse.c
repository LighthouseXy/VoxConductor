#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "app_sse.h"
int main(void) {
  app_sse_t s = {0};
  cJSON *event = NULL;
  assert(app_sse_event_with_json(&s, "{\"choices\":[],\"usage\":{\"total_tokens\":42}}", &event));
  assert(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(event, "usage"), "total_tokens")->valueint == 42);
  cJSON_Delete(event);
  assert(!app_sse_event_with_json(&s, "{\"error\":\"bad request\"}", &event));
  assert(cJSON_GetObjectItemCaseSensitive(event, "error"));
  cJSON_Delete(event);
  assert(!app_sse_event_with_json(&s, "{} trailing", &event));
  assert(!event);
  assert(!app_sse_event_with_json(&s, "{\"text\":\"\\u0000\"}", &event));
  assert(!event);
  assert(!app_sse_event(&s, "[DONE]"));
  assert(app_sse_event(&s, "{\"choices\":[{\"delta\":{\"content\":\"你好。\"},\"finish_reason\":null}]}"));
  assert(app_sse_speech_ready(&s, false)); // 普通问答不等全文
  assert(!app_sse_speech_ready(&s, true)); // 短前言留到分支明确，不等长回答全文
  assert(app_sse_event(&s, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}"));
  assert(app_sse_event(&s, "{\"choices\":[],\"usage\":{}}"));
  assert(app_sse_event(&s, "[DONE]")); assert(!strcmp(s.text, "你好。"));
  assert(app_sse_speech_ready(&s, true)); // 完整直接答复可以播放
  assert(!app_sse_event(&s, "[DONE]"));
  memset(&s, 0, sizeof(s));
  assert(app_sse_event(&s, "{\"choices\":[{\"delta\":{\"content\":\"你好。\"},\"finish_reason\":null}]}"));
  s.speech_committed = true;
  assert(!app_sse_event(&s, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"late\",\"function\":{\"name\":\"get_time\",\"arguments\":\"{}\"}}]}}]}"));
  assert(s.tool_count == 0); // 已播报后不能转换到工具分支
  memset(&s, 0, sizeof(s));
  assert(app_sse_event(&s, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"call1\",\"function\":{\"name\":\"get_time\",\"arguments\":\"{\"}}]}}]}"));
  assert(app_sse_event(&s, "{\"choices\":[{\"delta\":{\"content\":null,\"tool_calls\":[{\"index\":0,\"type\":null,\"id\":null,\"function\":{\"name\":null,\"arguments\":\"}\"}}]},\"finish_reason\":\"tool_calls\"}]}"));
  assert(app_sse_event(&s, "[DONE]"));
  assert(!app_sse_speech_ready(&s, true)); // 完整工具调用也不播放旁白
  assert(!app_sse_speech_ready(&s, false));
  cJSON *message = app_sse_tool_message(&s); assert(message);
  assert(cJSON_IsNull(cJSON_GetObjectItemCaseSensitive(message, "content")));
  cJSON_Delete(message);
  memset(&s, 0, sizeof(s));
  assert(!app_sse_event(&s, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"length\"}]}"));
  assert(!app_sse_event(&s, "{\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":9}]}}]}"));
  assert(!app_sse_event(&s, "{\"choices\":[{\"delta\":{\"content\":\"\\u0000\"}}]}"));
  memset(&s, 0, sizeof(s));
  assert(!app_sse_event(&s, "{\"choices\":[{\"delta\":{\"tool_calls\":\"invalid\"}}]}"));
  memset(&s, 0, sizeof(s));
  const char *piece = "{\"choices\":[{\"delta\":{\"content\":\"abcdefghijklmnop\"}}]}";
  for (unsigned i = 0; i < 256; ++i) assert(app_sse_event(&s, piece));
  assert(strlen(s.text) == 4096 && app_sse_speech_ready(&s, false));
  assert(app_sse_speech_ready(&s, true)); // 长回答可以在stop/DONE前播放
  assert(app_sse_event(&s, "{\"choices\":[{\"delta\":{},\"finish_reason\":\"stop\"}]}"));
  assert(app_sse_event(&s, "[DONE]"));
  puts("SSE text, split tool arguments and termination guards: PASS");
}
