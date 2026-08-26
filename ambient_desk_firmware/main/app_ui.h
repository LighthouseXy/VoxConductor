#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
  APP_UI_VOICE_LISTENING = 0,
  APP_UI_VOICE_THINKING,
  APP_UI_VOICE_SPEAKING,
  APP_UI_VOICE_ERROR,
} app_ui_voice_state_t;

// 在LVGL初始化完成后创建所有页面、覆盖层和UI定时器。
esp_err_t app_ui_init(void);

// 当前由LVGL按键定时器调用，因此调用时已经处在LVGL上下文中。
void app_ui_show_next_page(void);
void app_ui_show_previous_page(void);

// 可从普通FreeRTOS任务调用，内部负责获取LVGL锁。
void app_ui_show_voice_state(app_ui_voice_state_t state, const char *message);
void app_ui_hide_voice_state(void);

// 线程安全的数据入口；UI定时器负责把最新状态绘制到控件。
void app_ui_set_mic_level(uint32_t level);
void app_ui_set_wifi_connected(bool connected);
// 从普通FreeRTOS任务更新首页和天气页的天气数据。
void app_ui_set_weather(const char *city, int temperature_c,
                        int apparent_c, const char *condition,
                        int weather_code, const char *updated_at);
