/*
 * 实体按键消抖，区分短按、长按、双击/释放并产生事件。
 * 不在输入检测路径执行慢I/O，动作含义由当前页面决定。
 */
#include "app_button.h"

#include <stdatomic.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define APP_BUTTON_PIN GPIO_NUM_16
#define APP_BUTTON_POLL_MS 10
#define APP_BUTTON_STABLE_SAMPLES 3
#define APP_BUTTON_LONG_PRESS_MS 500
#define APP_BUTTON_DOUBLE_CLICK_MS 280

static const char *TAG = "app_button";

static app_button_event_callback_t s_callback;
static void *s_callback_user_data;
static int s_last_raw_level = 1;
static int s_stable_level = 1;
static uint8_t s_stable_samples;
static TickType_t s_press_start_tick;
static bool s_long_press_triggered;
static bool s_long_press_started;
static bool s_press_consumed;
static bool s_second_click;
static _Atomic bool s_click_pending;
static TickType_t s_click_release_tick;
static uint32_t s_press_edge_ms;
uint32_t app_button_press_time_ms(void) { return s_press_edge_ms; }

static bool emit_event(app_button_event_t event) {
  return s_callback != NULL && s_callback(event, s_callback_user_data);
}

bool app_button_click_pending(void) { return atomic_load(&s_click_pending); }

static void clear_click(void) {
  atomic_store(&s_click_pending, false);
  s_second_click = false;
}

static void finish_click(void) {
  // Keep the pending flag set through the callback: motion cannot change the
  // selected item while the button task is acquiring the LVGL lock.
  if (app_button_click_pending())
    emit_event(APP_BUTTON_EVENT_SHORT_PRESS);
  clear_click();
}

static void poll_button(void) {
  const int raw_level = gpio_get_level(APP_BUTTON_PIN);
  const TickType_t now = xTaskGetTickCount();
  if (raw_level == 1 && s_stable_level == 1 && app_button_click_pending() &&
      !s_second_click &&
      now - s_click_release_tick >= pdMS_TO_TICKS(APP_BUTTON_DOUBLE_CLICK_MS))
    finish_click();

  if (raw_level != s_last_raw_level) {
    if (raw_level == 0) s_press_edge_ms = (uint32_t)(esp_timer_get_time() / 1000);
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
      s_second_click = app_button_click_pending() &&
                       now - s_click_release_tick <
                           pdMS_TO_TICKS(APP_BUTTON_DOUBLE_CLICK_MS);
      if (app_button_click_pending() && !s_second_click)
        finish_click();
      s_press_start_tick = now;
      s_long_press_triggered = false;
      s_long_press_started = false;
      s_press_consumed = emit_event(APP_BUTTON_EVENT_PRESS_DOWN);
      if (s_press_consumed)
        clear_click();
      return;
    }

    const bool consumed = s_press_consumed;
    const bool was_long_press = s_long_press_triggered;
    const bool long_press_started = s_long_press_started;
    s_long_press_triggered = false;
    s_long_press_started = false;
    s_press_consumed = false;
    if (consumed)
      return;

    if (was_long_press) {
      if (long_press_started) {
        emit_event(APP_BUTTON_EVENT_LONG_PRESS_END);
      }
      return;
    }

    if (s_second_click && app_button_click_pending()) {
      emit_event(APP_BUTTON_EVENT_DOUBLE_PRESS);
      clear_click();
    } else {
      s_click_release_tick = now;
      atomic_store(&s_click_pending, true);
      if (emit_event(APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE))
        clear_click();
    }
    return;
  }

  if (s_stable_level != 0 || s_long_press_triggered || s_press_consumed) {
    return;
  }

  const TickType_t held_ticks = now - s_press_start_tick;
  if (held_ticks < pdMS_TO_TICKS(APP_BUTTON_LONG_PRESS_MS)) {
    return;
  }

  // A hold overrides an unfinished click pair; it cannot also confirm a menu.
  clear_click();
  s_long_press_triggered = true;
  s_long_press_started = emit_event(APP_BUTTON_EVENT_LONG_PRESS_START);
}

static void button_task(void *argument) {
  (void)argument;
  while (true) {
    poll_button();
    vTaskDelay(pdMS_TO_TICKS(APP_BUTTON_POLL_MS));
  }
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
  clear_click();
  s_last_raw_level = s_stable_level = 1;
  s_stable_samples = 0;
  s_long_press_triggered = s_long_press_started = s_press_consumed = false;

  if (xTaskCreate(button_task, "button", 4096, NULL, 7, NULL) != pdPASS) {
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(
      TAG,
      "按键已启用：GPIO%"
      "d，10ms采样，500ms长按，280ms双击；单击进入，双击返回，语音按下请求停止",
      APP_BUTTON_PIN);
  return ESP_OK;
}
