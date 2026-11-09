#include <assert.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#include "app_cloud.h"
#include "app_audio.h"
#include "app_barge_diagnostic.h"
#include "app_ui.h"
#include "app_tts_options.h"
#include "esp_websocket_client.h"
#include "freertos/event_groups.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

struct test_ws { test_ws_callback_t callback; void *context; pthread_mutex_t lock; };
struct test_events { _Atomic EventBits_t bits; };
static _Atomic bool cancelled;
static bool cancel_on_play, malformed, disconnect_on_append, no_audio, small_queue, fail_create;
static bool delayed_finish, require_finished, cancel_while_buffering;
static bool fail_update;
static _Atomic bool synthesis_finished;
static _Thread_local UBaseType_t task_priority = 3;
static unsigned priority_updates, required_play_priority;
static size_t audio_bytes;
static _Atomic size_t submitted;
static unsigned connections, closes, finishes, appends, aborts;
static _Atomic unsigned full_waits;
static char submitted_text[2048];
static pthread_t input_thread;
static bool input_thread_started;
static app_tts_options_t voice_options = {100, 0};
static app_tts_options_t expected_options = {100, 0};
static bool options_available = true, change_options_on_connect;
bool app_desktop_tts_options(app_tts_options_t *out) {
  *out = voice_options;
  return options_available && app_tts_options_valid(*out);
}
int64_t esp_timer_get_time(void) {
  struct timespec now; assert(clock_gettime(CLOCK_MONOTONIC, &now) == 0);
  return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}
void vTaskDelay(TickType_t ticks) {
  struct timespec delay = {.tv_sec = ticks / 1000, .tv_nsec = (long)(ticks % 1000) * 1000000};
  nanosleep(&delay, NULL);
}
void vTaskDelete(void *task) { (void)task; pthread_exit(NULL); }
UBaseType_t uxTaskPriorityGet(void *task) { assert(!task); return task_priority; }
void vTaskPrioritySet(void *task, UBaseType_t priority) {
  assert(!task); task_priority = priority; ++priority_updates;
}
typedef struct { void (*function)(void *); void *argument; } task_start_t;
static void *start_task(void *argument) {
  task_start_t start = *(task_start_t *)argument; free(argument);
  start.function(start.argument); return NULL;
}
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack, void *arg, unsigned priority, void *handle) {
  (void)stack; (void)handle;
  assert(priority == 5 && !strcmp(name, "tts_input") && !input_thread_started && !connections);
  if (fail_create) return 0;
  task_start_t *start = malloc(sizeof(*start)); assert(start);
  *start = (task_start_t){task, arg};
  if (pthread_create(&input_thread, NULL, start_task, start)) { free(start); return 0; }
  input_thread_started = true; return 1;
}
bool app_cloud_is_cancelled(void) { return cancelled; }
esp_err_t app_network_resolve_url(const char *url) { assert(url && !strncmp(url, "wss://", 6)); return ESP_OK; }
const char *esp_err_to_name(int error) { (void)error; return "mock"; }
void app_ui_show_voice_state(app_ui_voice_state_t state, const char *message) { (void)state; assert(message); }
void app_acoustic_playing(bool active) { (void)active; }
void app_barge_diagnostic_end(unsigned epoch, barge_diag_reason_t reason) { (void)epoch; (void)reason; }
bool app_acoustic_listen(bool active) { (void)active; return true; }
void app_turn_resources(const app_turn_t *turn, const char *point) { assert(turn && point); }
void app_turn_emit(app_turn_t *turn, app_turn_event_type_t type, const char *text, int error) {
  app_turn_event_t event; assert(app_turn_apply(turn, type, esp_timer_get_time(), text, error, &event));
}
void app_turn_emit_at(app_turn_t *turn, app_turn_event_type_t type, int64_t at) {
  app_turn_event_t event; assert(app_turn_apply(turn, type, at, NULL, ESP_OK, &event));
}
esp_err_t app_audio_play_mono_pcm(const int16_t *pcm, size_t count) {
  if (required_play_priority) assert(task_priority == required_play_priority);
  if (require_finished) assert(atomic_load(&synthesis_finished));
  assert(pcm && count && !cancelled); submitted += count * 2;
  if (small_queue) vTaskDelay(1);
  if (cancel_on_play) cancelled = true;
  return ESP_OK;
}
esp_err_t app_audio_abort_playback(void) { ++aborts; return ESP_OK; }
void app_audio_playback_diagnostics(bool reset) { (void)reset; }
EventGroupHandle_t xEventGroupCreate(void) { return calloc(1, sizeof(struct test_events)); }
EventBits_t xEventGroupSetBits(EventGroupHandle_t events, EventBits_t bits) { return atomic_fetch_or(&events->bits, bits) | bits; }
EventBits_t xEventGroupWaitBits(EventGroupHandle_t events, EventBits_t bits, int clear, int all, unsigned wait) {
  (void)clear; (void)all; (void)wait; return atomic_load(&events->bits) & bits;
}
void vEventGroupDelete(EventGroupHandle_t events) { free(events); }
StreamBufferHandle_t xStreamBufferCreateStatic(size_t size, size_t trigger, uint8_t *storage, StaticStreamBuffer_t *control) {
  assert(trigger == 1);
  *control = (StaticStreamBuffer_t){.storage = storage, .capacity = small_queue ? 32768 : size - 1};
  assert(!pthread_mutex_init(&control->lock, NULL) && !pthread_cond_init(&control->changed, NULL));
  return control;
}
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *data, size_t size, unsigned wait) {
  pthread_mutex_lock(&buffer->lock);
  if (buffer->bytes == buffer->capacity && wait) {
    ++full_waits;
    struct timespec deadline; clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += (long)wait * 1000000;
    deadline.tv_sec += deadline.tv_nsec / 1000000000; deadline.tv_nsec %= 1000000000;
    pthread_cond_timedwait(&buffer->changed, &buffer->lock, &deadline);
  }
  if (size > buffer->capacity - buffer->bytes) size = buffer->capacity - buffer->bytes;
  memcpy(buffer->storage + buffer->bytes, data, size); buffer->bytes += size;
  pthread_mutex_unlock(&buffer->lock); return size;
}
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *data, size_t size, unsigned wait) {
  (void)wait; pthread_mutex_lock(&buffer->lock);
  if (size > buffer->bytes) size = buffer->bytes;
  memcpy(data, buffer->storage, size); buffer->bytes -= size;
  memmove(buffer->storage, buffer->storage + size, buffer->bytes);
  pthread_cond_broadcast(&buffer->changed); pthread_mutex_unlock(&buffer->lock); return size;
}
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer) {
  pthread_mutex_lock(&buffer->lock); size_t bytes = buffer->bytes; pthread_mutex_unlock(&buffer->lock); return bytes;
}
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t buffer) {
  pthread_mutex_lock(&buffer->lock); size_t space = buffer->capacity - buffer->bytes;
  pthread_mutex_unlock(&buffer->lock); return space;
}
void vStreamBufferDelete(StreamBufferHandle_t buffer) {
  pthread_cond_destroy(&buffer->changed); pthread_mutex_destroy(&buffer->lock);
}
int mbedtls_base64_decode(unsigned char *out, size_t cap, size_t *written, const unsigned char *data, size_t size) {
  (void)data; (void)size; if (malformed) return -1;
  assert(audio_bytes <= cap); memset(out, 1, audio_bytes); *written = audio_bytes; return 0;
}
static void message(struct test_ws *client, const char *json) {
  esp_websocket_event_data_t data = {.op_code = 1, .fin = true, .payload_len = (int)strlen(json),
      .data_len = (int)strlen(json), .data_ptr = json};
  client->callback(client->context, NULL, WEBSOCKET_EVENT_DATA, &data);
}
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config) {
  assert(input_thread_started); // reserve sender before allocating TLS/client resources
  if (change_options_on_connect) voice_options = (app_tts_options_t){150, 6};
  assert(strstr(config->uri, "mock.invalid") && strstr(config->headers, "test-only"));
  ++connections; struct test_ws *client = calloc(1, sizeof(*client)); assert(client);
  assert(!pthread_mutex_init(&client->lock, NULL)); return client;
}
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, int event, test_ws_callback_t callback, void *context) {
  assert(event == WEBSOCKET_EVENT_ANY); client->callback = callback; client->context = context; return ESP_OK;
}
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client) {
  message(client, "{\"type\":\"session.created\"}"); return ESP_OK;
}
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int length, unsigned wait) {
  (void)wait;
  // 模拟组件默认共享锁：回调可在持锁时等待队列，但PCM消费者必须独立继续。
  pthread_mutex_lock(&client->lock);
  cJSON *root = cJSON_Parse(data); assert(root);
  const char *type = cJSON_GetObjectItem(root, "type")->valuestring;
  if (!strcmp(type, "session.update")) {
    cJSON *session = cJSON_GetObjectItem(root, "session");
    assert(!strcmp(cJSON_GetObjectItem(session, "voice")->valuestring,
                   app_cloud_tts_voice(expected_options.voice_id)));
    assert(cJSON_GetObjectItem(session, "speech_rate")->valuedouble ==
           expected_options.rate_percent / 100.0);
    if (fail_update) { cJSON_Delete(root); pthread_mutex_unlock(&client->lock); return -1; }
    message(client, "{\"type\":\"session.updated\",\"session\":{\"sample_rate\":24000,\"response_format\":\"pcm\"}}");
  }
  else if (!strcmp(type, "input_text_buffer.append")) {
    assert(!finishes && !cancelled); ++appends;
    const char *text = cJSON_GetObjectItem(root, "text")->valuestring;
    assert(strlen(submitted_text) + strlen(text) < sizeof(submitted_text)); strcat(submitted_text, text);
    if (disconnect_on_append) client->callback(client->context, NULL, WEBSOCKET_EVENT_DISCONNECTED, NULL);
    else if (!no_audio) {
      message(client, "{\"type\":\"response.audio.delta\",\"delta\":\"mock\"}");
      if (cancel_while_buffering) cancelled = true;
    }
  } else if (!strcmp(type, "session.finish")) {
    ++finishes; assert(finishes == 1);
    if (delayed_finish) vTaskDelay(50);
    atomic_store(&synthesis_finished, true);
    message(client, "{\"type\":\"session.finished\"}");
    client->callback(client->context, NULL, WEBSOCKET_EVENT_CLOSED, NULL);
  } else assert(0);
  cJSON_Delete(root); pthread_mutex_unlock(&client->lock); return length;
}
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client) {
  assert(client);
  if (input_thread_started) { assert(!pthread_join(input_thread, NULL)); input_thread_started = false; }
  ++closes; return ESP_OK;
}
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client) {
  pthread_mutex_destroy(&client->lock); free(client); return ESP_OK;
}
typedef struct { unsigned position; _Atomic bool abort; bool abort_feed; } feed_t;
static esp_err_t feed(void *context, char text[481], bool *final) {
  feed_t *input = context; text[0] = 0; *final = false;
  if (input->abort_feed && input->position) { atomic_store(&input->abort, true); return ESP_ERR_INVALID_STATE; }
  if (input->position == 1 && !atomic_load(&submitted)) return ESP_OK;
  if (input->position == 0) strcpy(text, "第一段。");
  if (input->position == 2) strcpy(text, "第二段。");
  if (input->position == 3) *final = true;
  ++input->position; return ESP_OK;
}
static app_turn_t fresh(void) {
  assert(!input_thread_started);
  cancelled = cancel_on_play = malformed = disconnect_on_append = no_audio = small_queue = fail_create = false;
  connections = closes = finishes = appends = aborts = 0; submitted = 0; submitted_text[0] = 0;
  audio_bytes = 30721; full_waits = 0;
  delayed_finish = require_finished = cancel_while_buffering = fail_update = false;
  synthesis_finished = false; task_priority = 3; priority_updates = required_play_priority = 0;
  voice_options = expected_options = (app_tts_options_t){100, 0};
  options_available = true; change_options_on_connect = false;
  app_turn_t turn; app_turn_init(&turn, "tts", 1, esp_timer_get_time());
  app_turn_emit(&turn, TURN_TTS_START, NULL, ESP_OK); return turn;
}
int main(void) {
  app_turn_t turn = fresh(); feed_t input = {0};
  assert(!app_tts_options_valid((app_tts_options_t){49, 0}));
  assert(!app_tts_options_valid((app_tts_options_t){201, 0}));
  assert(!app_tts_options_valid((app_tts_options_t){101, 0}));
  assert(!app_tts_options_valid((app_tts_options_t){100, APP_TTS_VOICE_COUNT}));
  assert(!app_cloud_tts_voice(APP_TTS_VOICE_COUNT));
  for (unsigned voice = 0; voice < APP_TTS_VOICE_COUNT; ++voice) {
    turn = fresh(); voice_options = expected_options = (app_tts_options_t){75, voice};
    audio_bytes = 30720;
    assert(app_cloud_speak("音色与语速测试", &turn) == ESP_OK);
  }
  turn = fresh(); voice_options = expected_options = (app_tts_options_t){200, 3};
  audio_bytes = 30720;
  change_options_on_connect = true;
  assert(app_cloud_speak("会话参数保持一致", &turn) == ESP_OK);
  turn = fresh(); voice_options = expected_options = (app_tts_options_t){50, 2};
  audio_bytes = 30720;
  assert(app_cloud_speak_reminder("提醒测试", &turn) == ESP_OK);
  turn = fresh(); options_available = false;
  assert(app_cloud_speak("配置读取失败", &turn) == ESP_ERR_INVALID_STATE && !connections);
  turn = fresh(); voice_options = (app_tts_options_t){100, 99};
  assert(app_cloud_speak("无效音色", &turn) == ESP_ERR_INVALID_STATE && !connections);
  turn = fresh();
  assert(app_cloud_speak_stream(feed, &input, &turn, &input.abort) == ESP_OK);
  assert(connections == 1 && closes == 1 && finishes == 1 && appends == 2 && aborts == 0);
  assert(!strcmp(submitted_text, "第一段。第二段。") && submitted == 2 * audio_bytes);
  assert(turn.tts_batches == 1 && turn.tts_text_chunks == 2 && turn.pcm_submitted_bytes == submitted);
  turn = fresh(); input = (feed_t){0}; small_queue = true; audio_bytes = 48000;
  assert(app_cloud_speak_stream(feed, &input, &turn, &input.abort) == ESP_OK);
  assert(full_waits > 0 && submitted == 96000 && appends == 2 && finishes == 1 && closes == 1);
  turn = fresh(); input = (feed_t){0}; cancel_on_play = true;
  assert(app_cloud_speak_stream(feed, &input, &turn, &input.abort) == ESP_ERR_INVALID_STATE);
  assert(connections == 1 && closes == 1 && aborts == 1 && appends == 1 && finishes == 0);
  turn = fresh(); input = (feed_t){.abort_feed = true};
  assert(app_cloud_speak_stream(feed, &input, &turn, &input.abort) == ESP_ERR_INVALID_STATE && !cancelled);
  assert(aborts == 1 && closes == 1 && !finishes);
  turn = fresh(); malformed = true;
  assert(app_cloud_speak("测试", &turn) == ESP_ERR_INVALID_RESPONSE && aborts == 1 && closes == 1);
  turn = fresh(); disconnect_on_append = true;
  assert(app_cloud_speak("测试", &turn) == ESP_FAIL && aborts == 1 && closes == 1);
  turn = fresh(); no_audio = true;
  assert(app_cloud_speak_reminder("提醒喝水", &turn) == ESP_ERR_INVALID_SIZE && aborts == 1);
  assert(task_priority == 3 && priority_updates == 2);
  turn = fresh(); audio_bytes = 1024;
  require_finished = delayed_finish = true; required_play_priority = 6;
  assert(app_cloud_speak_reminder("提醒喝水", &turn) == ESP_OK && submitted == 1024 && !aborts);
  assert(task_priority == 3 && priority_updates == 4);
  turn = fresh(); audio_bytes = 60000;
  require_finished = delayed_finish = true; required_play_priority = 6;
  assert(app_cloud_speak_reminder("时间到了，提醒你：喝水。", &turn) == ESP_OK);
  assert(submitted == 60000 && task_priority == 3 && turn.tts_starvations == 0);
  // 缓冲容量小于整段音频时必须起播腾出空间，不能只等session.finished。
  turn = fresh(); small_queue = true; audio_bytes = 48000; required_play_priority = 6;
  assert(app_cloud_speak_reminder("较长提醒", &turn) == ESP_OK);
  assert(submitted == 48000 && full_waits > 0 && finishes == 1 && task_priority == 3);
  turn = fresh(); audio_bytes = 1024; cancel_while_buffering = true;
  assert(app_cloud_speak_reminder("提醒喝水", &turn) == ESP_ERR_INVALID_STATE);
  assert(!submitted && aborts == 1 && task_priority == 3 && priority_updates == 2);
  turn = fresh(); audio_bytes = 1024; task_priority = required_play_priority = 6;
  assert(app_cloud_speak_reminder("提醒喝水", &turn) == ESP_OK);
  assert(task_priority == 6 && !priority_updates);
  turn = fresh(); fail_create = true;
  assert(app_cloud_speak("测试", &turn) == ESP_ERR_NO_MEM && !connections && !closes && !appends && !input_thread_started);
  turn = fresh(); fail_create = true;
  assert(app_cloud_speak_reminder("提醒喝水", &turn) == ESP_ERR_NO_MEM && !connections && !closes);
  assert(task_priority == 3 && priority_updates == 2);
  turn = fresh(); fail_update = true;
  assert(app_cloud_speak("测试", &turn) == ESP_FAIL && connections == 1 && closes == 1);
  assert(!appends && !finishes && !input_thread_started); // gated sender exits on handshake failure
  puts("TTS actual protocol: independent sender/full queue, one connection, single finish, odd PCM, cancellation, cleanup, reminder prefetch and priority: PASS");
}
