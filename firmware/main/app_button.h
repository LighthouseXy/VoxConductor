#pragma once

#include <stdbool.h>

#include "esp_err.h"

typedef enum {
  APP_BUTTON_EVENT_SHORT_PRESS = 0,
  APP_BUTTON_EVENT_LONG_PRESS_START,
  APP_BUTTON_EVENT_LONG_PRESS_END,
} app_button_event_t;

typedef bool (*app_button_event_callback_t)(app_button_event_t event,
                                            void *user_data);

// 初始化物理按键、消抖和长短按识别。回调在LVGL定时器上下文中执行。
esp_err_t app_button_init(app_button_event_callback_t callback,
                          void *user_data);
