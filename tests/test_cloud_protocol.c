#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_conversation.h"
#include "app_cloud_wire.h"

static void test_context(void) {
  app_conversation_t history = {0};
  for (int i = 0; i < 8; ++i) {
    char user[32], answer[32];
    snprintf(user, sizeof(user), "问题%d：\"你好\"\n", i);
    snprintf(answer, sizeof(answer), "回答%d", i);
    assert(app_conversation_add(&history, user, answer));
  }
  assert(history.count == 6);
  cJSON *messages = app_conversation_messages(&history, "继续介绍");
  assert(messages && cJSON_GetArraySize(messages) == 16);
  assert(strstr(history.summary, "问题0"));
  const cJSON *first = cJSON_GetArrayItem(messages, 3);
  assert(strcmp(cJSON_GetObjectItem(first, "content")->valuestring, "问题2：\"你好\"\n") == 0);
  const cJSON *last = cJSON_GetArrayItem(messages, 15);
  assert(strcmp(cJSON_GetObjectItem(last, "content")->valuestring, "继续介绍") == 0);
  for (int i = 3; i < 15; ++i) {
    const char *role = cJSON_GetObjectItem(cJSON_GetArrayItem(messages, i), "role")->valuestring;
    assert(strcmp(role, i % 2 ? "user" : "assistant") == 0);
  }
  char *json = cJSON_PrintUnformatted(messages);
  cJSON *roundtrip = cJSON_Parse(json);
  assert(roundtrip && cJSON_GetArraySize(roundtrip) == 16);
  free(json); cJSON_Delete(roundtrip); cJSON_Delete(messages);
  char oversized[APP_CONTEXT_TEXT_BYTES + 1];
  memset(oversized, 'x', sizeof(oversized) - 1); oversized[sizeof(oversized) - 1] = 0;
  app_conversation_t snapshot = history;
  assert(!app_conversation_add(&history, oversized, "ok"));
  assert(!app_conversation_add(&history, "ok", ""));
  assert(memcmp(&snapshot, &history, sizeof(history)) == 0);
  assert(!app_conversation_messages(&history, oversized));

  memset(&history, 0, sizeof(history));
  assert(app_conversation_reply_tokens("一句话解释原因") == 180);
  assert(app_conversation_reply_tokens("比较两种方案") == 400);
  assert(app_conversation_reply_tokens("详细解释") == 1600);
  assert(app_conversation_add(&history, "比较两种供电方案", "1.外接充电宝。2.内置电池模块。"));
  assert(app_conversation_add(&history, "第二个但不要依赖电脑", "选择内置电池模块。"));
  assert(strstr(history.discussion, "内置电池") && strstr(history.discussion, "不依赖") == NULL);
  assert(strstr(history.discussion, "不要依赖电脑") && strstr(history.discussion, "\"confirmed_choice\":2"));
  for (int i = 0; i < 8; ++i) assert(app_conversation_add(&history, "你好", "你好"));
  messages = app_conversation_messages(&history, "继续这个方案");
  char *selected = cJSON_PrintUnformatted(messages);
  assert(strstr(selected, "内置电池模块") && strstr(selected, "不要依赖电脑"));
  free(selected); cJSON_Delete(messages);
  // 无关长期条目不能挤掉近期原文，通用沟通偏好仍可常驻。
  strcpy(history.notes, "[{\"key\":\"饮品偏好\",\"value\":\"咖啡\",\"valid_until\":0},{\"key\":\"回答风格\",\"value\":\"中文\",\"valid_until\":0},{\"key\":\"城市\",\"value\":\"杭州\",\"valid_until\":1}]");
  messages = app_conversation_messages(&history, "为什么天空是蓝色的");
  selected = cJSON_PrintUnformatted(messages);
  assert(!strstr(selected, "咖啡") && strstr(selected, "中文") && !strstr(selected, "杭州"));
  free(selected); cJSON_Delete(messages);
  messages = app_conversation_messages(&history, "我喜欢什么饮料");
  selected = cJSON_PrintUnformatted(messages); assert(strstr(selected, "咖啡"));
  free(selected); cJSON_Delete(messages);
  assert(app_conversation_add(&history, "改选另一个", "改为充电宝。"));
  assert(strstr(history.discussion, "\"confirmed_choice\":1"));
  assert(app_conversation_add(&history, "继续这个方案", "[回答被打断，没有完整播放记录。]"));
  assert(strstr(history.discussion, "\"interrupted\":true"));
  assert(!strcmp(app_conversation_memory_key("编程语言偏好"), "编程语言偏好"));
  strcpy(history.notes, "[{\"key\":\"猫的名字\",\"value\":\"汤圆\",\"valid_until\":0},{\"key\":\"设备型号\",\"value\":\"ESP32-S3\",\"valid_until\":0}]");
  messages = app_conversation_messages(&history, "猫叫什么");
  selected = cJSON_PrintUnformatted(messages); assert(strstr(selected, "汤圆")); free(selected); cJSON_Delete(messages);
  messages = app_conversation_messages(&history, "esp32有什么约束");
  selected = cJSON_PrintUnformatted(messages); assert(strstr(selected, "ESP32-S3")); free(selected); cJSON_Delete(messages);

  memset(&history, 0, sizeof(history));
  assert(app_conversation_add(&history, "比较两种供电方案", "1.充电宝。2.内置电池。"));
  assert(app_conversation_add(&history, "选第二个，但不要依赖电脑，必须无线", "1.选择电池。2.添加充电模块。"));
  assert(strstr(history.discussion, "充电宝") && strstr(history.discussion, "内置电池"));
  assert(!strstr(history.discussion, "添加充电模块") && strstr(history.discussion, "\"confirmed_choice\":2"));
  assert(app_conversation_add(&history, "现在可以依赖电脑", "可以通过电脑供电。"));
  assert(!strstr(history.discussion, "不要依赖电脑") && strstr(history.discussion, "必须无线"));
  assert(strstr(history.discussion, "可以依赖电脑"));
  assert(app_conversation_add(&history, "不允许依赖电脑", "不依赖电脑。"));
  assert(!strstr(history.discussion, "现在可以依赖电脑") && strstr(history.discussion, "必须无线"));
  assert(app_conversation_add(&history, "改选另一个", "1.连接充电宝。2.检查输出。"));
  assert(strstr(history.discussion, "\"confirmed_choice\":1") && strstr(history.discussion, "内置电池"));
  assert(app_conversation_add(&history, "修改约束：必须使用USB供电", "按新约束讨论。"));
  assert(!strstr(history.discussion, "必须无线") && strstr(history.discussion, "必须使用USB供电"));

  memset(&history, 0, sizeof(history));
  strcpy(history.notes, "[{\"key\":\"设备一\",\"value\":\"供电A\",\"valid_until\":0},{\"key\":\"设备二\",\"value\":\"供电B\",\"valid_until\":0},{\"key\":\"设备三\",\"value\":\"供电C\",\"valid_until\":0},{\"key\":\"设备四\",\"value\":\"供电D\",\"valid_until\":0},{\"key\":\"设备五\",\"value\":\"供电E\",\"valid_until\":0},{\"key\":\"设备六\",\"value\":\"供电F\",\"valid_until\":0},{\"key\":\"回答偏好\",\"value\":\"简洁中文\",\"valid_until\":0}]");
  messages = app_conversation_messages(&history, "供电方案如何选择");
  selected = cJSON_PrintUnformatted(messages);
  assert(strstr(selected, "简洁中文") && strstr(selected, "供电A") && !strstr(selected, "供电F"));
  free(selected); cJSON_Delete(messages);

  // 长原文进入有标记摘录，来源保留；候选整理不接收执行授权字段。
  memset(&history, 0, sizeof(history));
  char long_answer[5000]; memset(long_answer, 'x', sizeof(long_answer) - 1); long_answer[4999] = 0;
  assert(app_conversation_add_source(&history, "详细讲解供电", long_answer, "long-source"));
  assert(strlen(history.turns[0].assistant) < APP_CONTEXT_TEXT_BYTES && strstr(history.turns[0].assistant, "长回答摘录"));
  assert(!strcmp(history.turns[0].source_turn_id, "long-source"));
  cJSON *candidate = cJSON_Parse("{\"goal\":\"选择供电\",\"constraints\":\"必须无线\",\"open_question\":\"允许充电宝吗\",\"options\":[\"充电宝\",\"内置电池\"],\"confirmed_choice\":2}");
  assert(app_conversation_refine_discussion(&history, candidate));
  assert(strstr(history.discussion, "必须无线") && strstr(history.discussion, "\"confirmed_choice\":2"));
  cJSON_AddBoolToObject(candidate, "executed", true); snapshot = history;
  assert(!app_conversation_refine_discussion(&history, candidate)); assert(!memcmp(&snapshot, &history, sizeof(history))); cJSON_Delete(candidate);
  memset(&history, 0, sizeof(history));
  assert(app_conversation_add(&history, "详细讲解供电步骤", "1.接好电源。2.检查输出。"));
  cJSON *discussion = cJSON_Parse(history.discussion);
  assert(discussion && !cJSON_GetObjectItem(discussion, "options")); cJSON_Delete(discussion);
  assert(app_conversation_add(&history, "第二个", "第二步是检查输出。"));
  assert(!strstr(history.discussion, "confirmed_choice"));
}

static void test_websocket(void) {
  cloud_ws_message_t s = {0};
  char buffer[65];
  // 一个帧跨多个网络回调。
  assert(cloud_ws_append(&s, buffer, 64, 1, true, 6, 0, "abc", 3) == 0);
  assert(cloud_ws_append(&s, buffer, 64, 1, true, 6, 3, "def", 3) == 1);
  assert(strcmp(buffer, "abcdef") == 0);
  // 一个消息跨两个WebSocket帧，第二个帧又跨两个回调。
  assert(cloud_ws_append(&s, buffer, 64, 1, false, 2, 0, "ab", 2) == 0);
  assert(cloud_ws_append(&s, buffer, 64, 0, true, 4, 0, "cd", 2) == 0);
  assert(cloud_ws_append(&s, buffer, 64, 0, true, 4, 2, "ef", 2) == 1);
  assert(strcmp(buffer, "abcdef") == 0);
  assert(cloud_ws_append(&s, buffer, 64, 0, true, 2, 0, "ab", 2) == -1);
  s = (cloud_ws_message_t){0};
  assert(cloud_ws_append(&s, buffer, 3, 1, true, 4, 0, "abcd", 4) == -1);
  s = (cloud_ws_message_t){0};
  assert(cloud_ws_append(&s, buffer, 64, 1, true, 6, 0, "abc", 3) == 0);
  assert(cloud_ws_append(&s, buffer, 64, 1, true, 6, 4, "ef", 2) == -1);
  // 每一个可能的回调切点，确保不复制丢失、重复或错误结束。
  const char *payload = "{\"type\":\"response.audio.delta\",\"delta\":\"YWJj\"}";
  size_t length = strlen(payload);
  for (size_t cut = 1; cut < length; ++cut) {
    s = (cloud_ws_message_t){0};
    assert(cloud_ws_append(&s, buffer, 64, 1, true, length, 0, payload, cut) == 0);
    assert(cloud_ws_append(&s, buffer, 64, 1, true, length, cut, payload + cut, length - cut) == 1);
    assert(strcmp(buffer, payload) == 0);
  }
}

static uint32_t u32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void test_audio(void) {
  uint8_t wav[44], pcm[640000];
  for (size_t i = 0; i < sizeof(pcm); ++i) pcm[i] = i;
  // 包括最终Base64块余数为0、1、2的情况和20秒上限。
  const size_t sizes[] = {2, 4, 6, 1492, 1494, 32000, sizeof(pcm)};
  for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
    size_t bytes = sizes[i];
    cloud_wav_header(wav, bytes, 16000);
    assert(u32(wav + 4) == bytes + 36 && u32(wav + 40) == bytes);
    assert(u32(wav + 24) == 16000 && u32(wav + 28) == 32000);
    assert(wav[20] == 1 && wav[22] == 1 && wav[34] == 16);
    uint8_t *assembled = malloc(bytes + 44);
    for (size_t pos = 0; pos < bytes + 44;) {
      size_t n = bytes + 44 - pos;
      if (n > 1536) n = 1536;
      cloud_audio_slice(assembled + pos, wav, pcm, pos, n);
      pos += n;
    }
    assert(memcmp(assembled, wav, 44) == 0);
    assert(memcmp(assembled + 44, pcm, bytes) == 0);
    free(assembled);
  }
}

int main(void) {
  test_context(); test_websocket(); test_audio();
  puts("PASS: context eviction/roles/UTF-8; WebSocket fragmentation/bounds; WAV streaming boundaries");
  return 0;
}
