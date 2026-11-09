#include "../firmware/main/app_button.c"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static int level = 1, created;
static TickType_t ticks;
int64_t esp_timer_get_time(void) { return (int64_t)ticks * 1000; }
static int short_count, double_count, start_count, end_count, immediate_count;
static bool accept_long = true, consume_down, immediate_release;
int gpio_get_level(int pin) {
  assert(pin == 16);
  return level;
}
esp_err_t gpio_config(const gpio_config_t *config) {
  assert(config->pin_bit_mask == (1ULL << 16));
  return ESP_OK;
}
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(TickType_t n) { ticks += n; }
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle) {
  (void)arg;
  (void)handle;
  assert(task == button_task && !strcmp(name, "button") && stack >= 4096 &&
         priority == 7);
  ++created;
  return pdPASS;
}
static bool callback(app_button_event_t e, void *arg) {
  (void)arg;
  if (e == APP_BUTTON_EVENT_PRESS_DOWN)
    return consume_down;
  if (e == APP_BUTTON_EVENT_SHORT_RELEASE_CANDIDATE) {
    if (immediate_release)
      ++immediate_count;
    return immediate_release;
  }
  if (e == APP_BUTTON_EVENT_SHORT_PRESS)
    ++short_count;
  if (e == APP_BUTTON_EVENT_DOUBLE_PRESS)
    ++double_count;
  if (e == APP_BUTTON_EVENT_LONG_PRESS_START) {
    ++start_count;
    return accept_long;
  }
  if (e == APP_BUTTON_EVENT_LONG_PRESS_END)
    ++end_count;
  return true;
}
static void sample(int value, int count) {
  level = value;
  for (int i = 0; i < count; ++i) {
    poll_button();
    ticks += 10;
  }
}
static void reset_input(void) {
  ticks = 0;
  level = 1;
  s_last_raw_level = s_stable_level = 1;
  s_stable_samples = 0;
  s_long_press_triggered = s_long_press_started = s_press_consumed = false;
  clear_click();
  short_count = double_count = start_count = end_count = immediate_count = 0;
  accept_long = true;
  consume_down = immediate_release = false;
}
static void click(void) {
  sample(0, 8);
  sample(1, 4);
}
int main(void) {
  assert(app_button_init(callback, NULL) == ESP_OK && created == 1);
  reset_input();
  sample(0, 1);
  sample(1, 40);
  assert(!short_count && !double_count && !app_button_click_pending());

  click();
  assert(!short_count && app_button_click_pending());
  sample(1, 35);
  assert(short_count == 1 && !double_count && !app_button_click_pending());
  click();
  sample(1, 35);
  assert(short_count == 2 && !double_count);

  reset_input();
  click();
  click();
  sample(1, 40);
  assert(double_count == 1 && !short_count && !app_button_click_pending());

  reset_input();
  click();
  sample(0, 35); // second press stays held past the double window
  assert(app_button_click_pending() && !short_count && !double_count);
  sample(1, 4);
  sample(1, 35);
  assert(double_count == 1 && !short_count && !start_count);

  reset_input();
  click();
  sample(0, 60);
  sample(1, 4);
  sample(1, 35);
  assert(start_count == 1 && end_count == 1 && !short_count && !double_count);
  assert(!app_button_click_pending()); // second hold overrides pending click

  reset_input();
  accept_long = false;
  sample(0, 60);
  sample(1, 4);
  sample(1, 35);
  assert(start_count == 1 && !end_count && !short_count && !double_count);

  reset_input();
  consume_down = true;
  ticks = 1230;
  sample(0, 4);
  assert(app_button_press_time_ms() == 1230); // 不包含后续20ms消抖等待。
  assert(s_press_consumed && !short_count && !app_button_click_pending());
  sample(0, 60);
  sample(1, 40);
  assert(!start_count && !end_count && !short_count && !double_count);

  reset_input();
  click();
  consume_down = true; // audio begins between clicks: discard menu click
  click();
  sample(1, 40);
  assert(!short_count && !double_count && !app_button_click_pending());

  reset_input();
  immediate_release = true;
  click();
  click();
  click();
  sample(1, 40);
  assert(immediate_count == 3 && !double_count && !short_count);
  assert(!app_button_click_pending());
  sample(0, 60);
  sample(1, 4);
  assert(start_count == 1 && end_count == 1 && immediate_count == 3);

  reset_input();
  ticks = (TickType_t)-150;
  click();
  sample(1, 35);
  assert(short_count == 1 && !double_count); // unsigned tick wrap
  puts("PASS: button debounce, delayed single, double, hold arbitration, "
       "immediate voice stop, fast game clicks and tick wrap");
}
