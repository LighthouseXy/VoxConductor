/*
 * 独立文本分段组件，当前CMake未启用，保留测试/实验用途。
 * 不代表生产路径并行运行LLM/TTS；生产编排以main/app_cloud为准。
 */
#include "app_speech.h"
#include "app_cloud.h"
#include "app_conversation.h"
#include "app_memory_alloc.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct app_speech {
  char text[4096]; // SPSC环形队列，输入任务取走后立即复用，不随回答长度增长。
  _Atomic size_t published, consumed;
  _Atomic bool final, abort, done;
  _Atomic int result;
  app_turn_t playback; // 播放独占副本，join后合并，不能与SSE任务并发改同一个turn。
  int64_t waiting_since;
};

size_t app_speech_chunk(const char *text, size_t bytes, bool final, bool first, unsigned wait_ms) {
  if (!text || !bytes) return 0;
  size_t limit = first ? 180 : 480;
  if (final && bytes <= limit) return bytes;
  if (!first && !final && bytes < limit && wait_ms < 250) return 0;
  size_t end = 0, n = bytes < limit ? bytes : limit;
  for (size_t i = 0; i < n; ++i) {
    size_t punctuation = 0;
    if (text[i] == '\n' || text[i] == '!' || text[i] == '?') punctuation = 1;
    else if (i + 3 <= n && (!memcmp(text + i, "。", 3) || !memcmp(text + i, "！", 3) ||
                           !memcmp(text + i, "？", 3))) punctuation = 3;
    if (punctuation) {
      end = i + punctuation;
      if (first && end >= 24) return end;
    }
  }
  if (end && (end >= 24 || final || wait_ms >= 250)) return end;
  if (bytes >= limit) {
    n = limit;
    while (n && n < bytes && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    return n;
  }
  // 无句末的慢流也不能永久等待；输入来自已解析的完整UTF-8 delta。
  return final || wait_ms >= 500 ? bytes : 0;
}

static esp_err_t feed_text(void *context, char text[481], bool *final) {
  app_speech_t *s = context;
  text[0] = 0;
  if (atomic_load(&s->abort) || app_cloud_is_cancelled()) return ESP_ERR_INVALID_STATE;
  bool ended = atomic_load(&s->final);
  size_t consumed = atomic_load(&s->consumed);
  size_t published = atomic_load(&s->published), available = published - consumed;
  if (!available) s->waiting_since = esp_timer_get_time();
  size_t window = available < 480 ? available : 480;
  for (size_t i = 0; i < window; ++i) text[i] = s->text[(consumed + i) % sizeof(s->text)];
  // 环形物理边界可以切开字符，但提交TTS的逻辑窗口必须以完整UTF-8结束。
  while (window && window < available && ((unsigned char)s->text[(consumed + window) % sizeof(s->text)] & 0xc0) == 0x80) --window;
  size_t take = app_speech_chunk(text, window, ended && window == available, !consumed,
      (unsigned)((esp_timer_get_time() - s->waiting_since) / 1000));
  text[take] = 0;
  atomic_store(&s->consumed, consumed + take);
  if (take) s->waiting_since = esp_timer_get_time();
  *final = ended && consumed + take == published;
  return ESP_OK;
}

static void speech_task(void *arg) {
  app_speech_t *s = arg;
  s->waiting_since = esp_timer_get_time();
  // 等首段而不提前占TLS；后续文本在同一个TTS会话中追加，不等待上一段播完。
  while (!atomic_load(&s->published) && !atomic_load(&s->final) &&
         !atomic_load(&s->abort) && !app_cloud_is_cancelled()) vTaskDelay(pdMS_TO_TICKS(10));
  esp_err_t result = ESP_OK;
  if (atomic_load(&s->published) && !atomic_load(&s->abort) && !app_cloud_is_cancelled()) {
    app_turn_emit(&s->playback, TURN_TTS_START, NULL, ESP_OK);
    result = app_cloud_speak_stream(feed_text, s, &s->playback, &s->abort);
    // 供应商没有逐字声学位置；中断时不能把已发送文本当作已播放。
    if (result == ESP_OK) s->playback.played_text_bytes = atomic_load(&s->consumed);
  }
  if (atomic_load(&s->abort) || app_cloud_is_cancelled()) result = ESP_ERR_INVALID_STATE;
  atomic_store(&s->result, result);
  // 最后一次访问s，调用者看到done后才可释放；任务随后仅删除自身。
  atomic_store(&s->done, true);
  vTaskDelete(NULL);
}

app_speech_t *app_speech_start(const app_turn_t *turn) {
  if (!turn || app_cloud_is_cancelled()) return NULL;
  app_speech_t *s = app_psram_zero(sizeof(*s));
  if (!s) return NULL;
  atomic_init(&s->published, 0);
  atomic_init(&s->consumed, 0);
  atomic_init(&s->final, false); atomic_init(&s->abort, false); atomic_init(&s->done, false);
  atomic_init(&s->result, ESP_OK);
  s->playback = *turn;
  if (xTaskCreate(speech_task, "tts_play", 8192, s, 5, NULL) != pdPASS) {
    free(s); return NULL;
  }
  return s;
}

esp_err_t app_speech_status(const app_speech_t *s) {
  if (!s) return ESP_OK;
  if (app_cloud_is_cancelled() || atomic_load(&s->abort)) return ESP_ERR_INVALID_STATE;
  return atomic_load(&s->result);
}

esp_err_t app_speech_append(app_speech_t *s, const char *text, size_t bytes) {
  if (!s || !text || atomic_load(&s->final)) return ESP_ERR_INVALID_ARG;
  esp_err_t result = app_speech_status(s);
  if (result != ESP_OK) return result;
  size_t length = atomic_load(&s->published);
  if (bytes >= APP_REPLY_TEXT_BYTES || length + bytes >= APP_REPLY_TEXT_BYTES) return ESP_ERR_INVALID_SIZE;
  int64_t deadline = esp_timer_get_time() + 10000000;
  while (bytes) {
    result = app_speech_status(s);
    if (result != ESP_OK) return result;
    if (esp_timer_get_time() > deadline) return ESP_ERR_TIMEOUT;
    size_t room = sizeof(s->text) - (length - atomic_load(&s->consumed));
    size_t n = bytes < room ? bytes : room;
    // 完整delta才发布；避免消费者把暂时的字节尾误认作完整UTF-8。
    while (n && n < bytes && ((unsigned char)text[n] & 0xc0) == 0x80) --n;
    if (!n) { vTaskDelay(pdMS_TO_TICKS(10)); continue; }
    for (size_t i = 0; i < n; ++i) s->text[(length + i) % sizeof(s->text)] = text[i];
    length += n; text += n; bytes -= n;
    atomic_store(&s->published, length);
  }
  return ESP_OK;
}

esp_err_t app_speech_finish(app_speech_t *s, app_turn_t *turn, bool success) {
  if (!s) return ESP_OK;
  if (!success) atomic_store(&s->abort, true);
  atomic_store(&s->final, true);
  while (!atomic_load(&s->done)) vTaskDelay(pdMS_TO_TICKS(10));
  esp_err_t result = atomic_load(&s->result);
  turn->event_sequence = s->playback.event_sequence;
  turn->stages[TURN_SYNTHESIZE] = s->playback.stages[TURN_SYNTHESIZE];
  turn->stages[TURN_PLAY] = s->playback.stages[TURN_PLAY];
  turn->times[TURN_TTS_START] = s->playback.times[TURN_TTS_START];
  turn->times[TURN_FIRST_PCM] = s->playback.times[TURN_FIRST_PCM];
  turn->times[TURN_PLAY_START] = s->playback.times[TURN_PLAY_START];
  turn->played_text_bytes = s->playback.played_text_bytes;
  turn->pcm_submitted_bytes = s->playback.pcm_submitted_bytes;
  turn->tts_batches = s->playback.tts_batches;
  turn->tts_text_chunks = s->playback.tts_text_chunks;
  turn->tts_starvations = s->playback.tts_starvations;
  turn->tts_wait_us = s->playback.tts_wait_us;
  free(s);
  return result;
}
