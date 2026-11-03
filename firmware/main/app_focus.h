#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
  int64_t deadline_us;
  unsigned remaining, work_minutes, break_minutes, cycles;
  bool running, pomodoro, resting, interrupted;
} app_focus_t;
void app_focus_start(app_focus_t *f, unsigned minutes, bool pomodoro,
                     int64_t now);
unsigned app_focus_remaining(const app_focus_t *f, int64_t now);
void app_focus_toggle(app_focus_t *f, int64_t now);
// 返回一次阶段完成事件；下一阶段等待用户主动继续。
bool app_focus_tick(app_focus_t *f, int64_t now);
