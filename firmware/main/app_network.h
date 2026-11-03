#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "app_weather.h"
bool app_network_weather_snapshot(app_weather_t *out, unsigned *age_seconds);

typedef struct {
  bool connected, time_valid, time_syncing, weather_updating;
  esp_err_t weather_error;
  unsigned time_retry_seconds, weather_retry_seconds;
} app_network_status_t;
void app_network_status_snapshot(app_network_status_t *out);

// 初始化NVS和Wi-Fi，不等待首连；启动维护任务后持续重连、校时和刷新天气。
esp_err_t app_network_init(void);
bool app_network_is_connected(void);
// Defer new background weather/AQ HTTPS while an owned voice session is active.
void app_network_voice_priority(bool active);
// Cache an IPv4 DNS result before cloud clients reserve internal worker stacks.
// No service-IP pinning, certificate bypass, or replay of completed requests.
esp_err_t app_network_resolve_url(const char *url);

esp_err_t app_network_fetch_weather(void);
esp_err_t app_network_start_weather_updates(void);
// 异步请求后台owner重新校时，不在调用线程初始化或等待SNTP。
esp_err_t app_network_sync_time(void);

// 上传一轮录音并边接收边播放服务器返回的PCM音频。
esp_err_t app_network_upload_voice_turn(const int16_t *recording,
                                        size_t recording_bytes,
                                        const char *turn_id);

// 获取本轮ASR文本和AI回答，并追加写入SD卡。
esp_err_t app_network_fetch_and_store_turn(const char *turn_id);
