#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
  APP_BUTTON_EVENT_SHORT_PRESS = 0,
  APP_BUTTON_EVENT_LONG_PRESS_START,
  APP_BUTTON_EVENT_LONG_PRESS_END,
  APP_BUTTON_EVENT_DOUBLE_PRESS,
  // true consumes this complete press; used for immediate voice cancellation.
  APP_BUTTON_EVENT_PRESS_DOWN,
  // true consumes a physical release (for example, immediate audio cancellation).
  // false defers SHORT_PRESS until the double-click window has closed.
  APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE,
} app_button_event_t;

typedef bool (*app_button_event_callback_t)(app_button_event_t event,
                                            void *user_data);

// 初始化物理按键、消抖和单/双击、长按识别。回调不持有LVGL锁。
esp_err_t app_button_init(app_button_event_callback_t callback,
                          void *user_data);
// 菜单等待单/双击判定期间，姿态选择应暂停，避免提交到另一选项。
bool app_button_click_pending(void);
// 仅在按键回调中读取：本次通过消抖的按下边沿时间，不含消抖/双击等待。
uint32_t app_button_press_time_ms(void);
