#include "app_motion_gesture.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>

static uint32_t now;
static int feed(motion_gesture_t *s, int delta, unsigned ms, bool valid,
                bool blocked) {
  int result = 0;
  for (unsigned i = 0; i < ms; i += 20) {
    now += 20;
    int event = motion_gesture_step(s, delta, now, valid, blocked);
    if (event) {
      assert(result == 0);
      result = event;
    }
  }
  return result;
}

static int feed_accel(motion_input_t *s, const int accel[3], unsigned ms,
                      bool valid, bool blocked) {
  int result = 0;
  for (unsigned i = 0; i < ms; i += 20) {
    now += 20;
    // 固件的同一入口，覆盖启动、后台校准、滤波和动作判断。
    int event = motion_input_step(s, accel, 1, 1, now, valid, blocked);
    if (event) {
      assert(result == 0);
      result = event;
    }
  }
  return result;
}

static void test_input_without_calibration(void) {
  motion_input_t s = {0};
  const int tilt[] = {125, 224, 956};
  const int left[] = {-59, -936, 394};
  const int flat[] = {31, 31, 1028};
  const int right[] = {-45, 998, 69};
  now = 0;
  assert(feed_accel(&s, tilt, 2000, true, false) == 0);
  assert(!s.calibrated && s.neutral == 0);
  // 没有静置校准，跨过中间也能左翻；持续倾斜不连翻。
  assert(feed_accel(&s, left, 2000, true, false) == -1);
  assert(feed_accel(&s, left, 2000, true, false) == 0);
  assert(feed_accel(&s, flat, 100, true, false) == 0);
  assert(feed_accel(&s, right, 2000, true, false) == 1);
  assert(!s.calibrated);
  assert(feed_accel(&s, left, 2000, true, false) == -1);
  assert(feed_accel(&s, flat, 1400, true, false) == 0);
  assert(s.calibrated && s.neutral == 31 && s.gesture.armed);
  assert(feed_accel(&s, right, 2000, true, false) == 1);
  assert(feed_accel(&s, left, 2000, true, false) == -1);
  assert(s.neutral == 31);
  const int noise[] = {31, 35, 1028};
  assert(feed_accel(&s, noise, 1200, true, false) == 0);
  assert(s.neutral == 31); // 微小噪声不反复重置中位。
  const int shifted[] = {31, 70, 1028};
  assert(feed_accel(&s, shifted, 2400, true, false) == 0);
  assert(abs(s.neutral - 70) <
         20); // 窗口可跨过姿态微调，20mg以内不再追逐噪声。
  assert(feed_accel(&s, left, 1200, true, true) == 0);
  assert(feed_accel(&s, right, 1200, true, false) == 0);
  assert(!s.gesture.armed);
  assert(feed_accel(&s, shifted, 400, true, false) == 0);
  assert(feed_accel(&s, left, 400, true, false) == -1);
  // 第一帧直接取真实读数，不能用初始滤波零值伪造一次回中。
  s = (motion_input_t){0};
  assert(feed_accel(&s, right, 2000, true, false) == 0);
  assert(!s.gesture.armed && s.filtered == right[1]);
  assert(feed_accel(&s, flat, 160, true, false) == 0);
  assert(feed_accel(&s, left, 1000, true, false) == -1);
}

static void test_background_calibration(void) {
  motion_recenter_t r = {0};
  int neutral = 0;
  const int flat[] = {-67, 25, 1065};
  const int tilted[] = {311, 534, 823};
  now = 0;
  for (int i = 0; i < 150; ++i) {
    now += 20;
    assert(!motion_calibrate_step(&r, tilted, 1, now, true, false, &neutral));
  }
  assert(neutral == 0 && r.count == 0);
  for (int i = 0; i < 50; ++i) {
    now += 20;
    assert(!motion_calibrate_step(&r, flat, 1, now, true, false, &neutral));
  }
  now += 20;
  assert(motion_calibrate_step(&r, flat, 1, now, true, false, &neutral));
  assert(neutral == 25);
  for (int reason = 0; reason < 4; ++reason) {
    for (int i = 0; i < 30; ++i) {
      now += 20;
      assert(!motion_calibrate_step(&r, flat, 1, now, true, false, &neutral));
    }
    const int moved[] = {80, 25, 1065};
    now += reason == 2 ? 1000 : 20;
    assert(!motion_calibrate_step(&r, reason == 3 ? moved : flat, 1, now,
                                  reason != 0, reason == 1, &neutral));
    assert(r.count <= 1); // 无效、语音、间断、移动均重新计时。
    r = (motion_recenter_t){0};
  }
  const int inverted[] = {-67, 25, -1065};
  assert(!motion_calibrate_step(&r, inverted, 1, now, true, false, &neutral));
  assert(r.count == 0);
  now = UINT32_MAX - 500;
  for (int i = 0; i < 50; ++i) {
    now += 20;
    assert(!motion_calibrate_step(&r, flat, 1, now, true, false, &neutral));
  }
  now += 20;
  assert(motion_calibrate_step(&r, flat, 1, now, true, false, &neutral));
}

static void test_gesture(void) {
  motion_gesture_t s = {0};
  now = 0;
  assert(feed(&s, 500, 500, true, false) == 0);
  assert(feed(&s, 0, 20, true, false) == 0);
  assert(s.armed);
  assert(feed(&s, -500, 40, true, false) == 0);
  assert(feed(&s, 0, 40, true, false) == 0);
  assert(feed(&s, -500, 60, true, false) == -1);
  assert(feed(&s, -500, 1000, true, false) == 0);
  assert(feed(&s, 500, 1000, true, false) == 1);
  assert(feed(&s, 0, 20, true, false) == 0);
  assert(feed(&s, -500, 60, true, false) == -1);
  // 冷却中记住过中位，但不能提前触发。
  assert(feed(&s, 0, 20, true, false) == 0);
  assert(feed(&s, 500, 300, true, false) == 0);
  assert(s.center_passed && !s.armed);
  assert(feed(&s, 500, 100, true, false) == 0);
  assert(feed(&s, 500, 60, true, false) == 1);
  for (int reason = 0; reason < 3; ++reason) {
    if (reason == 2)
      now += 1000;
    assert(feed(&s, -500, reason == 0 ? 140 : 20, reason != 0, reason == 1) ==
           0);
    assert(feed(&s, 500, 500, true, false) == 0);
    assert(!s.armed && !s.center_passed);
    assert(feed(&s, 0, 20, true, false) == 0);
    assert(feed(&s, -500, 100, true, false) == 0);
    assert(!s.armed);
    assert(feed(&s, 0, 500, true, false) == 0);
    assert(feed(&s, 500, 60, true, false) == 1);
  }
  s = (motion_gesture_t){.require_stable_center = true};
  now = UINT32_MAX - 200;
  assert(feed(&s, 161, 300, true, false) == 0);
  assert(feed(&s, 160, 120, true, false) == 0);
  assert(!s.armed);
  assert(feed(&s, 160, 20, true, false) == 0);
  assert(s.armed);
  assert(feed(&s, 219, 100, true, false) == 0);
  assert(feed(&s, 220, 40, true, false) == 0);
  assert(feed(&s, 220, 20, true, false) == 1);
  assert(feed(&s, -160, 380, true, false) == 0);
  assert(!s.armed);
  assert(feed(&s, -160, 20, true, false) == 0);
  assert(s.armed);
  assert(feed(&s, -219, 100, true, false) == 0);
  assert(feed(&s, -220, 60, true, false) == -1);
}

static void test_transient_invalid(void) {
  motion_gesture_t s = {0};
  now = UINT32_MAX - 40;
  assert(feed(&s, 0, 20, true, false) == 0);
  assert(s.armed);
  assert(feed(&s, -500, 40, true, false) == 0);
  assert(feed(&s, -2000, 40, false, false) == 0);
  assert(s.armed && !s.require_stable_center);
  // 异常前的40ms不能计入新的确认时间。
  assert(feed(&s, -500, 40, true, false) == 0);
  assert(feed(&s, -500, 20, true, false) == -1);
  assert(feed(&s, -500, 500, true, false) == 0);
  // 无效帧不能制造过中位，恢复到另一侧也不能凭跳变重新触发。
  assert(feed(&s, 0, 20, false, false) == 0);
  assert(feed(&s, 500, 200, true, false) == 0);
  assert(!s.armed && !s.center_passed);
  assert(feed(&s, 0, 140, true, false) == 0);
  assert(feed(&s, 500, 60, true, false) == 1);
  // 长异常即使恰在恢复有效数据时达到100ms，也必须回中。
  assert(feed(&s, 0, 500, true, false) == 0);
  assert(feed(&s, 2000, 100, false, false) == 0);
  assert(feed(&s, -500, 300, true, false) == 0);
  assert(!s.armed && s.require_stable_center);
}

static int feed_measured(motion_input_t *s, const int accel[3], unsigned ms) {
  int result = 0;
  for (unsigned i = 0; i < ms; i += 20) {
    now += 20;
    int event = motion_input_step(s, accel, 1, -1, now,
                                  motion_accel_valid(accel), false);
    if (event) {
      assert(!result);
      result = event;
    }
  }
  return result;
}

static void test_installed_board(void) {
  motion_input_t s = {0};
  const int flat[] = {60, 25, 1205};
  const int transition[] = {324, 431, 1270}; // 旧1.25g上限会拒绝这帧。
  const int spike[] = {0, 500, 1700};
  const int left[] = {122, 496, 985};
  const int right[] = {44, -464, 1045};
  const int zero[] = {0, 0, 0};
  const int low[] = {0, 0, 649}, high[] = {0, 0, 1501};
  assert(motion_accel_valid(flat) && motion_accel_valid(transition));
  assert(!motion_accel_valid(zero) && !motion_accel_valid(spike));
  assert(!motion_accel_valid(low) && !motion_accel_valid(high));
  now = 0;
  assert(feed_measured(&s, flat, 1400) == 0);
  for (int side = 0; side < 2; ++side) {
    for (int repeat = 0; repeat < 3; ++repeat) {
      assert(feed_measured(&s, flat, 600) == 0);
      assert(feed_measured(&s, transition, 20) == 0);
      assert(feed_measured(&s, spike, 40) == 0);
      assert(feed_measured(&s, side ? right : left, 500) == (side ? 1 : -1));
      assert(feed_measured(&s, side ? right : left, 1500) == 0);
    }
  }
}

static void test_screen_coordinates(void) {
  // 原映射(-1/-1)与统一反向映射(+1/+1)共用原始坐标和校准中位。
  const int flat[] = {60, 25, 1205};
  const int samples[][3] = {
      {60, 425, 1100}, {60, -375, 1100}, {-340, 25, 1100}, {460, 25, 1100}};
  const int expected[][2] = {{-400, 0}, {400, 0}, {0, 400}, {0, -400}};
  int x, y;
  motion_screen_coordinates(flat, 1, -1, -1, 25, 60, &x, &y);
  assert(x == 0 && y == 0);
  for (unsigned i = 0; i < 4; ++i) {
    motion_screen_coordinates(samples[i], 1, -1, -1, 25, 60, &x, &y);
    assert(x == expected[i][0] && y == expected[i][1]);
    motion_screen_coordinates(samples[i], 1, 1, 1, 25, 60, &x, &y);
    assert(x == -expected[i][0] && y == -expected[i][1]);
  }
  // 检测轴改成X时，连续输入不能仍硬编码使用Y。
  const int swapped[] = {425, 60, 1100};
  motion_screen_coordinates(swapped, 0, -1, -1, 25, 60, &x, &y);
  assert(x == -400 && y == 0);
  // 纵向符号可以独立校正，不改变已验证的左右方向。
  motion_screen_coordinates(samples[2], 1, -1, 1, 25, 60, &x, &y);
  assert(x == 0 && y == -400);
}

static void test_screen_calibration(void) {
  motion_input_t horizontal = {0};
  motion_recenter_t vertical = {0};
  int vertical_neutral = 0, adjusted, x, y;
  const int flat[] = {60, 25, 1205};
  now = 0;
  for (int i = 0; i < 51; ++i) {
    now += 20;
    assert(motion_input_step(&horizontal, flat, 1, -1, now, true, false) == 0);
    adjusted = vertical_neutral;
    bool updated =
        motion_calibrate_step(&vertical, flat, 0, now, true, false, &adjusted);
    if (i < 50)
      assert(!updated);
    else
      assert(updated);
    if (updated)
      vertical_neutral = adjusted;
  }
  assert(horizontal.calibrated && horizontal.neutral == 25);
  assert(vertical_neutral == 60);
  motion_screen_coordinates(flat, 1, -1, -1, horizontal.neutral,
                            vertical_neutral, &x, &y);
  assert(x == 0 && y == 0);

  // 禁用翻页/语音占用时不重新校准，但有效连续输入仍可按原中位发布。
  const int shifted[] = {100, 60, 1205};
  for (int i = 0; i < 100; ++i) {
    now += 20;
    assert(motion_input_step(&horizontal, shifted, 1, -1, now, true, true) ==
           0);
    assert(!motion_calibrate_step(&vertical, shifted, 0, now, true, true,
                                  &vertical_neutral));
  }
  assert(horizontal.neutral == 25 && vertical_neutral == 60);
  motion_screen_coordinates(shifted, 1, -1, -1, horizontal.neutral,
                            vertical_neutral, &x, &y);
  assert(x == -35 && y == -40);
  for (int i = 0; i < 51; ++i) {
    now += 20;
    assert(motion_input_step(&horizontal, shifted, 1, -1, now, true, false) ==
           0);
    adjusted = vertical_neutral;
    if (motion_calibrate_step(&vertical, shifted, 0, now, true, false,
                              &adjusted))
      vertical_neutral = adjusted;
  }
  assert(horizontal.neutral == 60 && vertical_neutral == 100);
  motion_screen_coordinates(shifted, 1, -1, -1, horizontal.neutral,
                            vertical_neutral, &x, &y);
  assert(x == 0 && y == 0);
}

static void test_requested_calibration(void) {
  motion_input_t horizontal = {
      .neutral = 25,
      .filtered = 400,
      .calibrated = true,
      .filter_initialized = true,
      .gesture = {.armed = true, .candidate = -1, .center_passed = true},
      .calibration = {.tracking = true, .sum = 100, .count = 4}};
  motion_recenter_t vertical = {.tracking = true, .sum = 200, .count = 4};
  int vertical_neutral = 60;
  motion_input_reset_calibration(&horizontal);
  vertical = (motion_recenter_t){0};
  assert(horizontal.neutral == 25); // 新窗口完成前保留旧零点。
  assert(!horizontal.calibrated && !horizontal.filter_initialized);
  assert(!horizontal.calibration.tracking && horizontal.calibration.count == 0);
  assert(!horizontal.gesture.armed && horizontal.gesture.candidate == 0);
  assert(horizontal.gesture.require_stable_center);

  const int tilted[] = {60, 425, 1100};
  now = 0;
  for (int i = 0; i < 100; ++i) {
    now += 20;
    assert(motion_input_step(&horizontal, tilted, 1, -1, now, true, false) ==
           0);
    assert(!motion_calibrate_step(&vertical, tilted, 0, now, true, false,
                                  &vertical_neutral));
  }
  assert(!horizontal.calibrated && horizontal.neutral == 25);
  assert(vertical_neutral == 60);

  const int flat[] = {65, 30, 1205};
  // 手动校准重开完整1秒窗口；即使只差5mg也采用新中位。
  for (int i = 0; i < 51; ++i) {
    now += 20;
    assert(motion_input_step(&horizontal, flat, 1, -1, now, true, false) == 0);
    bool updated = motion_calibrate_step(&vertical, flat, 0, now, true, false,
                                         &vertical_neutral);
    assert(updated == (i == 50));
    if (i < 50)
      assert(!horizontal.calibrated);
  }
  assert(horizontal.calibrated && horizontal.neutral == 30);
  assert(vertical_neutral == 65);
  int x, y;
  motion_screen_coordinates(flat, 1, -1, -1, horizontal.neutral,
                            vertical_neutral, &x, &y);
  assert(x == 0 && y == 0);
}

int main(void) {
  test_gesture();
  test_background_calibration();
  test_input_without_calibration();
  test_transient_invalid();
  test_installed_board();
  test_screen_coordinates();
  test_screen_calibration();
  test_requested_calibration();
  puts("PASS: motion uncalibrated startup/bidirectional input "
       "pipeline/background calibration/screen coordinates/two-axis "
       "neutral/requested stable "
       "calibration/hold/debounce/cooldown/voice/errors/time wrap");
}
