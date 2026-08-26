#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

// 初始化NVS和Wi-Fi，并等待首次连接结果。
esp_err_t app_network_init(void);
bool app_network_is_connected(void);

esp_err_t app_network_fetch_weather(void);
esp_err_t app_network_start_weather_updates(void);
esp_err_t app_network_sync_time(void);

// 上传一轮录音并边接收边播放服务器返回的PCM音频。
esp_err_t app_network_upload_voice_turn(const int16_t *recording,
                                        size_t recording_bytes,
                                        const char *turn_id);

// 获取本轮ASR文本和AI回答，并追加写入SD卡。
esp_err_t app_network_fetch_and_store_turn(const char *turn_id);
