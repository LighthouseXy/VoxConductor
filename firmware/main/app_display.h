#pragma once

#include "esp_err.h"

// 初始化 ST7789、SPI 总线和 LVGL 显示驱动。
esp_err_t app_display_init(void);
