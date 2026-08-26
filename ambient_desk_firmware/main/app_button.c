#include "app_button.h"

#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

#define APP_BUTTON_PIN GPIO_NUM_1
#define APP_BUTTON_POLL_MS 20
#define APP_BUTTON_STABLE_SAMPLES 3
#define APP_BUTTON_LONG_PRESS_MS 500

static const char *TAG = "app_button";

static app_button_event_callback_t s_callback;
static void *s_callback_user_data;
static int s_last_raw_level = 1;
static int s_stable_level = 1;
static uint8_t s_stable_samples;
static TickType_t s_press_start_tick;
static bool s_long_press_triggered;
static bool s_long_press_started;

static bool emit_event(app_button_event_t event) {
  return s_callback != NULL && s_callback(event, s_callback_user_data);
}

static void poll_button(lv_timer_t *timer) {
  (void)timer;

  const int raw_level = gpio_get_level(APP_BUTTON_PIN);

  if (raw_level != s_last_raw_level) {
    s_last_raw_level = raw_level;
    s_stable_samples = 1;
    return;
  }

  if (s_stable_samples < APP_BUTTON_STABLE_SAMPLES) {
    s_stable_samples++;
    if (s_stable_samples < APP_BUTTON_STABLE_SAMPLES) {
      return;
    }
  }

  if (raw_level != s_stable_level) {
    s_stable_level = raw_level;

    if (s_stable_level == 0) {
      s_press_start_tick = xTaskGetTickCount();
      s_long_press_triggered = false;
      s_long_press_started = false;
      return;
    }

    const bool was_long_press = s_long_press_triggered;
    const bool long_press_started = s_long_press_started;
    s_long_press_triggered = false;
    s_long_press_started = false;

    if (was_long_press) {
      if (long_press_started) {
        emit_event(APP_BUTTON_EVENT_LONG_PRESS_END);
      }
      return;
    }

    emit_event(APP_BUTTON_EVENT_SHORT_PRESS);
    return;
  }

  if (s_stable_level != 0 || s_long_press_triggered) {
    return;
  }

  const TickType_t held_ticks = xTaskGetTickCount() - s_press_start_tick;
  if (held_ticks < pdMS_TO_TICKS(APP_BUTTON_LONG_PRESS_MS)) {
    return;
  }

  s_long_press_triggered = true;
  s_long_press_started = emit_event(APP_BUTTON_EVENT_LONG_PRESS_START);
}

esp_err_t app_button_init(app_button_event_callback_t callback,
                          void *user_data) {
  if (callback == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  const gpio_config_t config = {
      .pin_bit_mask = 1ULL << APP_BUTTON_PIN,
      .mode = GPIO_MODE_INPUT,
      .pull_up_en = GPIO_PULLUP_ENABLE,
      .pull_down_en = GPIO_PULLDOWN_DISABLE,
      .intr_type = GPIO_INTR_DISABLE,
  };

  esp_err_t result = gpio_config(&config);
  if (result != ESP_OK) {
    return result;
  }

  s_callback = callback;
  s_callback_user_data = user_data;

  if (!lvgl_port_lock(0)) {
    return ESP_ERR_TIMEOUT;
  }

  lv_timer_t *timer = lv_timer_create(poll_button, APP_BUTTON_POLL_MS, NULL);
  lvgl_port_unlock();

  if (timer == NULL) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "按键已启用：短按切页，长按说话，松开发送");
  return ESP_OK;
}
