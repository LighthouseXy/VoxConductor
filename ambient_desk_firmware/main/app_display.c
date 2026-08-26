#include "app_display.h"

#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#define LCD_HOST SPI2_HOST
#define LCD_WIDTH 240
#define LCD_HEIGHT 240
#define LCD_DRAW_BUFFER_LINES 20

#define LCD_PIN_DC GPIO_NUM_4
#define LCD_PIN_RST GPIO_NUM_5
#define LCD_PIN_MOSI GPIO_NUM_6
#define LCD_PIN_SCLK GPIO_NUM_7

#define LCD_SPI_CLOCK_HZ (40 * 1000 * 1000)

static const char *TAG = "app_display";

esp_err_t app_display_init(void) {
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

  esp_err_t result =
      spi_bus_initialize(LCD_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (result != ESP_OK) {
    return result;
  }

  esp_lcd_panel_io_handle_t io_handle = NULL;
  const esp_lcd_panel_io_spi_config_t io_config = {
      // 当前屏幕没有引出CS，并独占这组SPI线路。
      .cs_gpio_num = -1,
      .dc_gpio_num = LCD_PIN_DC,
      .spi_mode = 3,
      .pclk_hz = LCD_SPI_CLOCK_HZ,
      .trans_queue_depth = 1,
      .lcd_cmd_bits = 8,
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
      // ST7789内部为240x320显存，当前240x240模组需要80行偏移。
      (result = esp_lcd_panel_set_gap(panel_handle, 0, 80)) != ESP_OK ||
      (result = esp_lcd_panel_invert_color(panel_handle, true)) != ESP_OK ||
      (result = esp_lcd_panel_disp_on_off(panel_handle, true)) != ESP_OK) {
    return result;
  }

  const lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
  result = lvgl_port_init(&lvgl_config);
  if (result != ESP_OK) {
    return result;
  }

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
              .mirror_x = false,
              // 抵消当前棱镜结构造成的上下翻转。
              .mirror_y = true,
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
    ESP_LOGE(TAG, "LVGL显示器注册失败");
    return ESP_FAIL;
  }

  lv_display_set_default(display);
  ESP_LOGI(TAG, "ST7789和LVGL显示驱动初始化完成");
  return ESP_OK;
}
