#pragma once

#include "esp_err.h"

// 初始化 ST7789、SPI 总线和 LVGL 显示驱动。
esp_err_t app_display_init(void);
// GPIO9原生PWM背光；0关闭、100为默认亮度。仅初始化成功后可设置。
esp_err_t app_display_set_brightness(unsigned percent);
unsigned app_display_get_brightness(void);
