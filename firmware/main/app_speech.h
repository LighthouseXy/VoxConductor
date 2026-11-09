#pragma once
#include <stdbool.h>
#include <stddef.h>
#include "app_turn.h"
#include "esp_err.h"

typedef struct app_speech app_speech_t;
// SSE生产者/TTS输入消费者；4KiB有界滑动队列，单轮最多16383字节。
app_speech_t *app_speech_start(const app_turn_t *turn);
esp_err_t app_speech_append(app_speech_t *speech, const char *text, size_t bytes);
esp_err_t app_speech_status(const app_speech_t *speech);
// 必须调用：取消/失败也等待播放任务退出再释放，返回后旧音频不会复活。
esp_err_t app_speech_finish(app_speech_t *speech, app_turn_t *turn, bool success);
size_t app_speech_chunk(const char *text, size_t bytes, bool final, bool first, unsigned wait_ms);
