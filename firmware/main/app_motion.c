/*
 * 姿态采样、校准与倾斜事件生成，UI消费方向/强度。
 * 传感器不可用时报告降级，实体按键仍可独立操作。
 */
#include "app_motion.h"
#include "app_log.h"
#include "app_motion_gesture.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <stdlib.h>

_Static_assert(APP_MOTION_AXIS >= 0 && APP_MOTION_AXIS <= 1,
               "Level calibration requires X or Y motion axis");
_Static_assert(APP_MOTION_DIRECTION == 1 || APP_MOTION_DIRECTION == -1,
               "Invalid direction");
_Static_assert(APP_MOTION_VERTICAL_DIRECTION == 1 ||
                   APP_MOTION_VERTICAL_DIRECTION == -1,
               "Invalid vertical direction");

static const char *TAG = "app_motion";
static const char *s_model = "MPU";
static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;
static app_motion_callback_t s_callback;
static _Atomic bool s_enabled = true;
static _Atomic bool s_calibration_requested;
static _Atomic bool s_calibrating;
static portMUX_TYPE s_sample_lock = portMUX_INITIALIZER_UNLOCKED;
static int s_x, s_y;
static int64_t s_sample_us;
bool app_motion_sample(int *x_mg, int *y_mg) {
  if (!x_mg || !y_mg)
    return false;
  portENTER_CRITICAL(&s_sample_lock);
  *x_mg = s_x;
  *y_mg = s_y;
  int64_t stamp = s_sample_us;
  portEXIT_CRITICAL(&s_sample_lock);
  int64_t age = esp_timer_get_time() - stamp;
  return stamp && age >= 0 && age <= 200000;
}

bool app_motion_available(void) {
  int x_mg, y_mg;
  return app_motion_sample(&x_mg, &y_mg);
}

bool app_motion_request_calibration(void) {
  if (!app_motion_available())
    return false;
  atomic_store(&s_calibrating, true);
  atomic_store(&s_calibration_requested, true);
  return true;
}

bool app_motion_calibrating(void) { return atomic_load(&s_calibrating); }

static void invalidate_sample(void) {
  portENTER_CRITICAL(&s_sample_lock);
  s_sample_us = 0;
  portEXIT_CRITICAL(&s_sample_lock);
}

void app_motion_set_enabled(bool enabled) { atomic_store(&s_enabled, enabled); }

static esp_err_t read_registers(uint8_t reg, uint8_t *data, size_t size) {
  return i2c_master_transmit_receive(s_device, &reg, 1, data, size, 50);
}

static esp_err_t write_register(uint8_t reg, uint8_t value) {
  uint8_t bytes[] = {reg, value};
  return i2c_master_transmit(s_device, bytes, sizeof(bytes), 50);
}

static void release_bus(void) {
  if (s_device) {
    i2c_master_bus_rm_device(s_device);
    s_device = NULL;
  }
  if (s_bus) {
    i2c_del_master_bus(s_bus);
    s_bus = NULL;
  }
}

static void motion_task(void *argument) {
  (void)argument;
  motion_input_t input = {0};
  motion_recenter_t vertical_calibration = {0};
  int vertical_neutral = 0;
  bool vertical_calibrated = false;
  unsigned failures = 0;
#if APP_MOTION_DIAGNOSTICS
  uint32_t diagnostic_at = 0;
  unsigned invalid_count = 0, gap_count = 0;
  ESP_LOGI(TAG,
           "motion_diag: every 500ms; axis=%d polarity=%d trigger=%dmg "
           "center=%dmg confirm=%dms center_hold=%dms cooldown=%dms; "
           "counters=since_previous_log",
           APP_MOTION_AXIS, APP_MOTION_DIRECTION, MOTION_TRIGGER_MG,
           MOTION_CENTER_MG, MOTION_CONFIRM_MS, MOTION_CENTER_MS,
           MOTION_COOLDOWN_MS);
#endif
  ESP_LOGI(TAG,
           "%s手势已启用：轴=%d，默认中位="
           "0mg；经过中间即可左右倾斜翻页，放平静置可后台校准",
           s_model, APP_MOTION_AXIS);
  while (true) {
    if (atomic_exchange(&s_calibration_requested, false)) {
      motion_input_reset_calibration(&input);
      vertical_calibration = (motion_recenter_t){0};
      vertical_calibrated = false;
      atomic_store(&s_calibrating, true);
    }
    uint8_t bytes[6];
    esp_err_t result = read_registers(0x3B, bytes, sizeof(bytes));
    uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
    if (result != ESP_OK) {
      invalidate_sample();
#if APP_MOTION_DIAGNOSTICS
      ESP_LOGW(TAG, "motion_diag: read_error=%s consecutive=%u",
               esp_err_to_name(result), failures + 1);
#endif
      const int empty[3] = {0};
      motion_input_step(&input, empty, APP_MOTION_AXIS, APP_MOTION_DIRECTION,
                        now, false, true);
      motion_calibrate_step(&vertical_calibration, empty, 1 - APP_MOTION_AXIS,
                            now, false, true, &vertical_neutral);
      if (++failures >= 5) {
        atomic_store(&s_calibrating, false);
        atomic_store(&s_calibration_requested, false);
        ESP_LOGW(TAG, "%s连续读取失败，已停用摇动翻页；检查接线后重启：%s",
                 s_model, esp_err_to_name(result));
        release_bus();
        vTaskDelete(NULL);
      }
      vTaskDelay(pdMS_TO_TICKS(100));
      continue;
    }
    failures = 0;
    int accel[3];
    for (int i = 0; i < 3; ++i) {
      int16_t raw = (int16_t)(((uint16_t)bytes[2 * i] << 8) | bytes[2 * i + 1]);
      accel[i] = (int32_t)raw * 1000 / 16384; // ±2g：16384 LSB/g
    }
    bool valid = motion_accel_valid(accel);
    // 从不可用恢复时必须稳定回中，不沿用异常前的待触发动作。
    bool recovering = valid && !app_motion_available();
    if (recovering)
      input.gesture = (motion_gesture_t){.require_stable_center = true};
    if (!valid)
      invalidate_sample();
    int old_neutral = input.neutral;
    bool was_calibrated = input.calibrated;
    bool enabled = atomic_load(&s_enabled);
#if APP_MOTION_DIAGNOSTICS
    uint32_t sample_ms =
        input.gesture.seen_sample ? now - input.gesture.last_sample : 0;
    if (!valid)
      ++invalid_count;
    if (sample_ms > 100)
      ++gap_count;
#endif
    int direction =
        motion_input_step(&input, accel, APP_MOTION_AXIS, APP_MOTION_DIRECTION,
                          now, valid, !enabled);
    // 与水平轴相同：仅放平、连续稳定1秒且未被语音占用时更新静态中位。
    int adjusted_vertical = vertical_neutral;
    bool vertical_updated =
        motion_calibrate_step(&vertical_calibration, accel, 1 - APP_MOTION_AXIS,
                              now, valid, !enabled, &adjusted_vertical);
    if (vertical_updated && (!vertical_calibrated ||
                             abs(adjusted_vertical - vertical_neutral) >= 20)) {
      vertical_neutral = adjusted_vertical;
      vertical_calibrated = true;
    }
    bool calibrating = atomic_load(&s_calibrating);
    if (calibrating && !atomic_load(&s_calibration_requested) &&
        input.calibrated && vertical_calibrated) {
      atomic_store(&s_calibrating, false);
      input.gesture = (motion_gesture_t){.require_stable_center = true};
      direction = 0;
    }
    if (valid) {
      int x_mg, y_mg;
      motion_screen_coordinates(accel, APP_MOTION_AXIS, APP_MOTION_DIRECTION,
                                APP_MOTION_VERTICAL_DIRECTION, input.neutral,
                                vertical_neutral, &x_mg, &y_mg);
      // 禁用翻页时仍更新连续样本，但不在禁用期间重新校准。
      portENTER_CRITICAL(&s_sample_lock);
      s_x = x_mg;
      s_y = y_mg;
      s_sample_us = esp_timer_get_time();
      portEXIT_CRITICAL(&s_sample_lock);
    }
#if APP_MOTION_DIAGNOSTICS
    if ((uint32_t)(now - diagnostic_at) >= 500) {
      ESP_LOGI(TAG,
               "motion_diag: filtered=%d neutral=%d delta=%d dt=%lums valid=%d "
               "enabled=%d level=%d calibrated=%d armed=%d passed=%d "
               "stable_required=%d centering=%d candidate=%d cooldown=%d "
               "invalid_n=%u gap_n=%u",
               input.filtered, input.neutral,
               (input.filtered - input.neutral) * APP_MOTION_DIRECTION,
               (unsigned long)sample_ms, valid, enabled, motion_is_level(accel),
               input.calibrated, input.gesture.armed,
               input.gesture.center_passed, input.gesture.require_stable_center,
               input.gesture.centering, input.gesture.candidate,
               input.gesture.fired && (uint32_t)(now - input.gesture.fired_at) <
                                          MOTION_COOLDOWN_MS,
               invalid_count, gap_count);
      diagnostic_at = now;
      invalid_count = gap_count = 0;
    }
#endif
    if ((!was_calibrated && input.calibrated) || old_neutral != input.neutral)
      APP_LOG_DETAIL(TAG, "后台中位校准：轴=%d，中位=%d -> %dmg", APP_MOTION_AXIS,
               old_neutral, input.neutral);
    if (direction && !calibrating && !atomic_load(&s_calibrating)) {
      s_callback(direction);
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

esp_err_t app_motion_init(app_motion_callback_t callback) {
  if (!callback)
    return ESP_ERR_INVALID_ARG;
  if (s_bus)
    return ESP_ERR_INVALID_STATE;
  invalidate_sample();
  atomic_store(&s_calibrating, false);
  atomic_store(&s_calibration_requested, false);
  const i2c_master_bus_config_t config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = APP_MOTION_SDA_GPIO,
      .scl_io_num = APP_MOTION_SCL_GPIO,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .flags.enable_internal_pullup = true,
  };
  esp_err_t result = i2c_new_master_bus(&config, &s_bus);
  if (result != ESP_OK)
    return result;

  // 仅探测两种合法地址，短暂重试以容忍上电瞬态，不阻塞启动数秒。
  int address = -1;
  for (int attempt = 0; attempt < 3 && address < 0; ++attempt) {
    for (int candidate = 0x68; candidate <= 0x69; ++candidate) {
      result = i2c_master_probe(s_bus, candidate, 50);
      if (result == ESP_OK) {
        address = candidate;
        break;
      }
      if (result != ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "MPU地址0x%02X探测失败：%s", candidate,
                 esp_err_to_name(result));
        goto fail;
      }
    }
    if (address < 0 && attempt < 2)
      vTaskDelay(pdMS_TO_TICKS(100));
  }
  if (address < 0) {
    ESP_LOGW(TAG, "MPU地址0x68/0x69均无应答：SDA=%d，SCL=%d",
             APP_MOTION_SDA_GPIO, APP_MOTION_SCL_GPIO);
    goto fail;
  }
  const i2c_device_config_t device = {
      .dev_addr_length = I2C_ADDR_BIT_LEN_7,
      .device_address = address,
      .scl_speed_hz = 100000,
  };
  result = i2c_master_bus_add_device(s_bus, &device, &s_device);
  if (result != ESP_OK)
    goto fail;
  uint8_t identity = 0;
  result = read_registers(0x75, &identity, 1);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "地址有应答，但WHO_AM_I读取失败：%s",
             esp_err_to_name(result));
    goto fail;
  }
  ESP_LOGD(TAG, "MPU身份检查：地址=0x%02X，WHO_AM_I=0x%02X", address, identity);
  if (identity != 0x68 && identity != 0x70) {
    ESP_LOGW(TAG, "不支持的器件ID=0x%02x，仅支持MPU6050(0x68)/MPU6500(0x70)",
             identity);
    result = ESP_ERR_NOT_SUPPORTED;
    goto fail;
  }
  s_model = identity == 0x70 ? "MPU6500" : "MPU6050";
  if ((result = write_register(0x6B, 0x80)) != ESP_OK)
    goto fail;
  vTaskDelay(pdMS_TO_TICKS(100));
  if ((result = write_register(0x6B, 0x01)) != ESP_OK || // 唤醒并选择PLL时钟
      (result = write_register(0x6C, 0x00)) != ESP_OK ||
      (result = write_register(0x1A, 0x03)) !=
          ESP_OK || // 6050同时设置加速度滤波；6500仅陀螺滤波
      (result = write_register(0x19, 19)) != ESP_OK || // 1kHz/(19+1)=50Hz
      (result = write_register(0x1C, 0x00)) != ESP_OK)
    goto fail; // 加速度±2g
  // MPU6500独立加速度滤波：不旁路DLPF，带宽41Hz；1kHz/(19+1)=50Hz。
  // MPU6050的0x1D含义不同，不能向它写入此配置。
  if (identity == 0x70 && (result = write_register(0x1D, 0x03)) != ESP_OK)
    goto fail;

  // 实机自检：读回量程/滤波配置，避免仅凭身份ID就宣告配置成功。
  uint8_t accel_config[2] = {0};
  result = read_registers(0x1C, accel_config, identity == 0x70 ? 2 : 1);
  if (result != ESP_OK)
    goto fail;
  if (accel_config[0] != 0x00 ||
      (identity == 0x70 && accel_config[1] != 0x03)) {
    ESP_LOGE(TAG, "%s配置读回不符：ACCEL_CONFIG=0x%02X，下一寄存器=0x%02X",
             s_model, accel_config[0], accel_config[1]);
    result = ESP_ERR_INVALID_RESPONSE;
    goto fail;
  }
  ESP_LOGD(TAG, "%s加速度配置读回通过：量程±2g%s", s_model,
           identity == 0x70 ? "，ACCEL_CONFIG2=0x03" : "");
  vTaskDelay(pdMS_TO_TICKS(100));
  s_callback = callback;
  if (xTaskCreate(motion_task, "motion", 4096, NULL, 3, NULL) != pdPASS) {
    result = ESP_ERR_NO_MEM;
    goto fail;
  }
  ESP_LOGI(TAG, "%s连接成功：地址=0x%02x，SDA=%d，SCL=%d", s_model, address,
           APP_MOTION_SDA_GPIO, APP_MOTION_SCL_GPIO);
  return ESP_OK;
fail:
  release_bus();
  return result;
}
