/*
 * 屏幕硬件/LVGL移植：总线、背光、旋转和绘图/控件内存。
 * DMA兼容内存和PSRAM控件池用途不同，PSRAM不能替代全部内部RAM。
 */
#include "app_display.h"
#include "app_memory_alloc.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_DRAW_BUFFER_LINES 20
#define LCD_WIDGET_POOL_BYTES (64U * 1024U)
#define LCD_WIDGET_POOL_COUNT 3U

_Static_assert(LCD_WIDGET_POOL_BYTES <= LV_MEM_SIZE + LV_MEM_POOL_EXPAND_SIZE,
               "Widget pool exceeds the configured LVGL TLSF pool limit");

#define LCD_PIN_DC GPIO_NUM_11
#define LCD_PIN_RST GPIO_NUM_12
#define LCD_PIN_MOSI GPIO_NUM_10
#define LCD_PIN_SCLK GPIO_NUM_13
#define LCD_PIN_BL GPIO_NUM_9

#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)
#define LCD_BL_PWM_HZ 5000
#define LCD_BL_PWM_MAX (1U << 10)

static const char *TAG = "app_display";
// 本工程音频使用I2S，未占用LEDC；背光独占低速timer/channel 0。
static _Atomic bool s_ready;
static _Atomic unsigned s_brightness = 100;
static StaticSemaphore_t s_brightness_mutex;
static SemaphoreHandle_t s_brightness_lock;

esp_err_t app_display_set_brightness(unsigned percent) {
  if (percent > 100)
    return ESP_ERR_INVALID_ARG;
  if (!atomic_load(&s_ready))
    return ESP_ERR_INVALID_STATE;
  const uint32_t duty = (LCD_BL_PWM_MAX * percent + 50U) / 100U;
  // 固定占空比不启用fade服务；静态互斥量串行化原生set/update配对。
  xSemaphoreTake(s_brightness_lock, portMAX_DELAY);
  esp_err_t result = ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
  if (result == ESP_OK)
    result = ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
  if (result == ESP_OK)
    atomic_store(&s_brightness, percent);
  xSemaphoreGive(s_brightness_lock);
  return result;
}

unsigned app_display_get_brightness(void) { return atomic_load(&s_brightness); }

esp_err_t app_display_init(void) {
  if (atomic_load(&s_ready))
    return ESP_ERR_INVALID_STATE;
  // 新PCB：GPIO9经R22驱动SS8050基极，高电平开启背光。
  // 先关背光，面板和LVGL初始化成功后再打开。
  esp_err_t result = gpio_set_level(LCD_PIN_BL, 0);
  if (result != ESP_OK)
    return result;
  const gpio_config_t backlight_config = {
      .pin_bit_mask = 1ULL << LCD_PIN_BL,
      .mode = GPIO_MODE_OUTPUT,
      .pull_up_en = GPIO_PULLUP_DISABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };
  result = gpio_config(&backlight_config);
  if (result != ESP_OK)
    return result;
  const ledc_timer_config_t backlight_timer = {
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .duty_resolution = LEDC_TIMER_10_BIT,
      .timer_num = LEDC_TIMER_0,
      .freq_hz = LCD_BL_PWM_HZ,
      .clk_cfg = LEDC_AUTO_CLK,
  };
  result = ledc_timer_config(&backlight_timer);
  if (result != ESP_OK)
    return result;
  const ledc_channel_config_t backlight_channel = {
      .gpio_num = LCD_PIN_BL,
      .speed_mode = LEDC_LOW_SPEED_MODE,
      .channel = LEDC_CHANNEL_0,
      .timer_sel = LEDC_TIMER_0,
      .duty = 0,
      .hpoint = 0,
  };
  result = ledc_channel_config(&backlight_channel);
  if (result != ESP_OK)
    return result;
  s_brightness_lock = xSemaphoreCreateMutexStatic(&s_brightness_mutex);
  if (!s_brightness_lock)
    return ESP_ERR_NO_MEM;

  const size_t max_transfer_size =
      LCD_WIDTH * LCD_DRAW_BUFFER_LINES * sizeof(uint16_t);

  ESP_LOGI(TAG, "初始化SPI总线");

  const spi_bus_config_t bus_config = {
      .sclk_io_num = LCD_PIN_SCLK,
      .mosi_io_num = LCD_PIN_MOSI,
      .miso_io_num = -1,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = max_transfer_size,
  };

  result = spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (result != ESP_OK) {
    return result;
  }

  esp_lcd_panel_io_handle_t io_handle = NULL;
  const esp_lcd_panel_io_spi_config_t io_config = {
      // 新PCB的J2第8脚(CS)经R20接地，显示独占SPI2。
      .cs_gpio_num = -1,           .dc_gpio_num = LCD_PIN_DC, .spi_mode = 3,
      .pclk_hz = LCD_SPI_CLOCK_HZ, .trans_queue_depth = 1,    .lcd_cmd_bits = 8,
      .lcd_param_bits = 8,
  };

  result = esp_lcd_new_panel_io_spi(LCD_HOST, &io_config, &io_handle);
  if (result != ESP_OK) {
    return result;
  }

  esp_lcd_panel_handle_t panel_handle = NULL;
  const esp_lcd_panel_dev_config_t panel_config = {
      .reset_gpio_num = LCD_PIN_RST,
      .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
      .data_endian = LCD_RGB_DATA_ENDIAN_LITTLE,
      .bits_per_pixel = 16,
  };

  result = esp_lcd_new_panel_st7789(io_handle, &panel_config, &panel_handle);
  if (result != ESP_OK) {
    return result;
  }

  if ((result = esp_lcd_panel_reset(panel_handle)) != ESP_OK ||
      (result = esp_lcd_panel_init(panel_handle)) != ESP_OK ||
      // 240x240可视窗口：MY由1改为0后，行偏移由80同步改为0。
      (result = esp_lcd_panel_set_gap(panel_handle, 0, 0)) != ESP_OK ||
      (result = esp_lcd_panel_invert_color(panel_handle, true)) != ESP_OK ||
      (result = esp_lcd_panel_disp_on_off(panel_handle, true)) != ESP_OK) {
    return result;
  }

  const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
  result = lvgl_port_init(&lvgl_config);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "LVGL任务初始化失败：%s", esp_err_to_name(result));
    return result;
  }

  // LVGL TLSF的单池上限由LV_MEM_SIZE决定，不能直接注册192KiB池。
  // 保留192KiB PSRAM，分成3个64KiB池；内部/DMA缓冲保持不变。
  // 整块存储在成功注册后保留到复位，注册失败时在LVGL锁内逆序撤销。
  const size_t widget_bytes = LCD_WIDGET_POOL_BYTES * LCD_WIDGET_POOL_COUNT;
  uint8_t *widget_memory = app_psram_malloc(widget_bytes);
  if (!widget_memory) {
    ESP_LOGE(TAG,
             "界面PSRAM申请失败：需要=%zu B，空闲=%zu B，最大连续=%zu B",
             widget_bytes,
             heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT),
             heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    return ESP_ERR_NO_MEM;
  }
  if (!lvgl_port_lock(1000)) {
    ESP_LOGE(TAG, "界面内存池注册失败：等待LVGL锁超时");
    free(widget_memory);
    return ESP_ERR_TIMEOUT;
  }
  lv_mem_pool_t pools[LCD_WIDGET_POOL_COUNT] = {0};
  unsigned added = 0;
  for (; added < LCD_WIDGET_POOL_COUNT; ++added) {
    pools[added] =
        lv_mem_add_pool(widget_memory + added * LCD_WIDGET_POOL_BYTES,
                        LCD_WIDGET_POOL_BYTES);
    if (!pools[added])
      break;
  }
  if (added != LCD_WIDGET_POOL_COUNT) {
    ESP_LOGE(TAG,
             "LVGL内存池注册失败：第%u/%u池，单池=%u B，内置=%u B，扩展=%u B",
             added + 1, LCD_WIDGET_POOL_COUNT, LCD_WIDGET_POOL_BYTES,
             (unsigned)LV_MEM_SIZE, (unsigned)LV_MEM_POOL_EXPAND_SIZE);
    while (added)
      lv_mem_remove_pool(pools[--added]);
    lvgl_port_unlock();
    free(widget_memory);
    return ESP_ERR_NO_MEM;
  }
  lvgl_port_unlock();
  ESP_LOGI(TAG, "LVGL界面内存池就绪：%u个%u KiB PSRAM池，内置池=%u KiB",
           LCD_WIDGET_POOL_COUNT, LCD_WIDGET_POOL_BYTES / 1024U,
           (unsigned)(LV_MEM_SIZE / 1024U));

  const lvgl_port_display_cfg_t display_config = {
      .io_handle = io_handle,
      .panel_handle = panel_handle,
      .buffer_size = LCD_WIDTH * LCD_DRAW_BUFFER_LINES,
      .double_buffer = false,
      .hres = LCD_WIDTH,
      .vres = LCD_HEIGHT,
      .monochrome = false,
      .color_format = LV_COLOR_FORMAT_RGB565,
      .rotation =
          {
              .swap_xy = false,
              // 新外壳/棱镜安装方向：相对旧配置旋转180度，保留反射补偿。
              .mirror_x = true,
              .mirror_y = false,
          },
      .flags =
          {
              .buff_dma = true,
              .buff_spiram = false,
              .sw_rotate = false,
              .swap_bytes = false,
          },
  };

  lv_display_t *display = lvgl_port_add_disp(&display_config);
  if (display == NULL) {
    ESP_LOGE(TAG, "LVGL显示器注册失败：检查内部/DMA内存与绘图缓冲");
    return ESP_FAIL;
  }

  lv_display_set_default(display);
  atomic_store(&s_ready, true);
  result = app_display_set_brightness(100);
  if (result != ESP_OK) {
    atomic_store(&s_ready, false);
    return result;
  }
  ESP_LOGI(TAG,
           "ST7789和LVGL初始化完成：MOSI=%d，SCLK=%d，DC=%d，RST=%d，BL=%d",
           LCD_PIN_MOSI, LCD_PIN_SCLK, LCD_PIN_DC, LCD_PIN_RST, LCD_PIN_BL);
  ESP_LOGI(TAG, "显示方向：mirror_x=1，mirror_y=0，Y偏移=0");
  return ESP_OK;
}
