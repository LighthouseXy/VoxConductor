/*
 * 专注/番茄计时纯状态逻辑，单调时间驱动剩余秒数。
 * 刷新频率不改变计时速度，持久化和通知由调用层负责。
 */
#include "app_focus.h"
unsigned app_focus_remaining(const app_focus_t *f, int64_t now) {
  if (!f->running)
    return f->remaining;
  return f->deadline_us > now
             ? (unsigned)((f->deadline_us - now + 999999) / 1000000)
             : 0;
}
void app_focus_start(app_focus_t *f, unsigned minutes, bool pomodoro,
                     int64_t now) {
  if (minutes < 1 || minutes > 120)
    minutes = 25;
  f->pomodoro = pomodoro;
  f->resting = false;
  f->interrupted = false;
  f->remaining = minutes * 60;
  f->running = true;
  f->deadline_us = now + (int64_t)f->remaining * 1000000;
}
void app_focus_toggle(app_focus_t *f, int64_t now) {
  if (f->running) {
    f->remaining = app_focus_remaining(f, now);
    f->running = false;
  } else if (f->remaining) {
    f->deadline_us = now + (int64_t)f->remaining * 1000000;
    f->running = true;
    f->interrupted = false;
  }
}
bool app_focus_tick(app_focus_t *f, int64_t now) {
  if (!f->running || app_focus_remaining(f, now))
    return false;
  f->running = false;
  f->remaining = 0;
  if (!f->resting)
    f->cycles++;
  if (f->pomodoro) {
    f->resting = !f->resting;
    unsigned mins = f->resting ? f->break_minutes : f->work_minutes;
    if (!mins || mins > 120)
      mins = f->resting ? 5 : 25;
    f->remaining = mins * 60;
  }
  return true;
}
