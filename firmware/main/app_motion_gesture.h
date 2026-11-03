#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

// 单位mg，相对校准中位；回中范围小于触发门槛，避免边缘反复触发。
#define MOTION_TRIGGER_MG 220
#define MOTION_CENTER_MG 160
#define MOTION_CONFIRM_MS 40
#define MOTION_CENTER_MS 120
#define MOTION_COOLDOWN_MS 400
#define MOTION_RECENTER_MS 1000
#define MOTION_LEVEL_MG 180
#define MOTION_STABLE_MG 60
// 实测静置约1.2g；该范围用于动作可信性判定，传感器精度另行校准。
#define MOTION_VALID_MIN_MG 650
#define MOTION_VALID_MAX_MG 1500
#define MOTION_INVALID_RESET_MS 100

static inline bool motion_accel_valid(const int accel[3]) {
  int64_t norm_squared = 0;
  for (int i = 0; i < 3; ++i)
    norm_squared += (int64_t)accel[i] * accel[i];
  return norm_squared >= (int64_t)MOTION_VALID_MIN_MG * MOTION_VALID_MIN_MG &&
         norm_squared <= (int64_t)MOTION_VALID_MAX_MG * MOTION_VALID_MAX_MG;
}

// 连续滚动和手势翻页共用水平检测轴/符号；另一水平轴映射为屏幕纵轴。
// 两轴中位都是原始传感器坐标，先减中位再转换，避免镜像后再减原始偏置。
static inline void motion_screen_coordinates(const int accel[3], int axis,
                                             int horizontal_direction,
                                             int vertical_direction,
                                             int horizontal_neutral,
                                             int vertical_neutral, int *x_mg,
                                             int *y_mg) {
  *x_mg = (accel[axis] - horizontal_neutral) * horizontal_direction;
  *y_mg = (accel[1 - axis] - vertical_neutral) * vertical_direction;
}

typedef struct {
  bool armed;
  bool centering;
  bool seen_sample;
  int candidate;
  uint32_t candidate_since;
  uint32_t center_since;
  uint32_t fired_at;
  uint32_t last_sample;
  bool fired;
  bool center_passed;
  bool require_stable_center;
  int last_delta;
  bool invalid_tracking;
  uint32_t invalid_since;
} motion_gesture_t;

typedef struct {
  bool tracking;
  int anchor[3];
  int sum;
  unsigned count;
  uint32_t since;
  uint32_t last_sample;
} motion_recenter_t;

// 当前安装以Z正向朝上为放平；启动与运行时恢复使用相同姿态条件。
static inline bool motion_is_level(const int accel[3]) {
  return abs(accel[0]) <= MOTION_LEVEL_MG && abs(accel[1]) <= MOTION_LEVEL_MG &&
         accel[2] >= 850;
}

static inline bool motion_calibrate_step(motion_recenter_t *s,
                                         const int accel[3], int axis,
                                         uint32_t now, bool valid,
                                         bool inhibited, int *neutral) {
  if (!valid || inhibited || axis < 0 || axis > 1 || !motion_is_level(accel)) {
    *s = (motion_recenter_t){0};
    return false;
  }
  bool stable = s->tracking && (uint32_t)(now - s->last_sample) <= 100;
  for (int i = 0; i < 3; ++i)
    if (abs(accel[i] - s->anchor[i]) > MOTION_STABLE_MG)
      stable = false;
  if (!stable) {
    s->tracking = true;
    s->since = now;
    s->sum = 0;
    s->count = 0;
    for (int i = 0; i < 3; ++i)
      s->anchor[i] = accel[i];
  }
  s->last_sample = now;
  s->sum += accel[axis];
  ++s->count;
  if ((uint32_t)(now - s->since) < MOTION_RECENTER_MS)
    return false;
  *neutral = s->sum / (int)s->count;
  *s = (motion_recenter_t){0};
  return true;
}

// 返回-1上一页、+1下一页、0无动作。delta为已滤波的相对中位加速度。
static inline int motion_gesture_step(motion_gesture_t *s, int delta,
                                      uint32_t now, bool valid,
                                      bool inhibited) {
  bool gap = s->seen_sample && (uint32_t)(now - s->last_sample) > 100;
  bool crossed_center =
      valid && !s->invalid_tracking && s->seen_sample &&
      ((s->last_delta < -MOTION_CENTER_MG && delta > MOTION_CENTER_MG) ||
       (s->last_delta > MOTION_CENTER_MG && delta < -MOTION_CENTER_MG));
  s->seen_sample = true;
  s->last_sample = now;
  if (valid)
    s->last_delta = delta;
  if (!valid && !s->invalid_tracking) {
    s->invalid_tracking = true;
    s->invalid_since = now;
  }
  bool sustained_invalid =
      s->invalid_tracking &&
      (uint32_t)(now - s->invalid_since) >= MOTION_INVALID_RESET_MS;
  if (inhibited || gap || sustained_invalid) {
    s->armed = false;
    s->centering = false;
    s->candidate = 0;
    s->center_passed = false;
    s->require_stable_center = true;
    if (valid)
      s->invalid_tracking = false;
    return 0;
  }
  if (!valid) {
    // 短暂超限不消费待触发状态；无效帧不触发、不计入持续确认/回中。
    s->candidate = 0;
    s->centering = false;
    return 0;
  }
  s->invalid_tracking = false;
  if (!s->armed) {
    // 正常启动/翻页后只需经过中间，不要求停住。冷却期内经过也会被记住。
    // 持续数据异常或语音结束仍需稳定回中，不能凭一次跳变恢复。
    if (!s->require_stable_center) {
      if (abs(delta) <= MOTION_CENTER_MG || crossed_center)
        s->center_passed = true;
      if (s->center_passed &&
          (!s->fired || (uint32_t)(now - s->fired_at) >= MOTION_COOLDOWN_MS))
        s->armed = true;
      return 0;
    }
    if (abs(delta) <= MOTION_CENTER_MG) {
      if (!s->centering) {
        s->centering = true;
        s->center_since = now;
      }
      if ((uint32_t)(now - s->center_since) >= MOTION_CENTER_MS &&
          (!s->fired || (uint32_t)(now - s->fired_at) >= MOTION_COOLDOWN_MS)) {
        s->armed = true;
        s->require_stable_center = false;
      }
    } else
      s->centering = false;
    return 0;
  }
  int direction = delta >= MOTION_TRIGGER_MG    ? 1
                  : delta <= -MOTION_TRIGGER_MG ? -1
                                                : 0;
  if (!direction) {
    s->candidate = 0;
    return 0;
  }
  if (s->candidate != direction) {
    s->candidate = direction;
    s->candidate_since = now;
    return 0;
  }
  if ((uint32_t)(now - s->candidate_since) < MOTION_CONFIRM_MS)
    return 0;
  s->armed = false;
  s->centering = false;
  s->candidate = 0;
  s->fired = true;
  s->center_passed = false;
  s->require_stable_center = false;
  s->fired_at = now;
  return direction;
}

// 固件和主机测试共用整条采样处理链，避免只测试状态机而漏掉启动门槛。
typedef struct {
  motion_gesture_t gesture;
  motion_recenter_t calibration;
  int neutral;
  int filtered;
  bool filter_initialized;
  bool calibrated;
} motion_input_t;

// 保留旧中位直至新窗口完成，防止等待校准时坐标瞬时跳到默认零点。
static inline void motion_input_reset_calibration(motion_input_t *s) {
  s->calibration = (motion_recenter_t){0};
  s->calibrated = false;
  s->filter_initialized = false;
  s->gesture = (motion_gesture_t){.require_stable_center = true};
}

static inline int motion_input_step(motion_input_t *s, const int accel[3],
                                    int axis, int polarity, uint32_t now,
                                    bool valid, bool inhibited) {
  if (axis < 0 || axis > 1 || (polarity != 1 && polarity != -1))
    return 0;
  int adjusted = s->neutral;
  bool updated = motion_calibrate_step(&s->calibration, accel, axis, now, valid,
                                       inhibited, &adjusted);
  // 默认0mg对应当前水平安装方式；没有完成静置校准也照常识别动作。
  // 已校准后仅修正至少20mg的变化，避免微小噪声反复重置手势。
  if (updated && (!s->calibrated || abs(adjusted - s->neutral) >= 20)) {
    s->neutral = adjusted;
    s->filtered = adjusted;
    s->filter_initialized = true;
    s->calibrated = true;
    s->gesture = (motion_gesture_t){0};
  }
  if (valid) {
    bool gap = s->gesture.seen_sample &&
               (uint32_t)(now - s->gesture.last_sample) > 100;
    if (!s->filter_initialized || gap)
      s->filtered = accel[axis];
    else
      s->filtered += (accel[axis] - s->filtered) / 4;
    s->filter_initialized = true;
  } else
    s->filter_initialized = false;
  return motion_gesture_step(&s->gesture, (s->filtered - s->neutral) * polarity,
                             now, valid, inhibited);
}
