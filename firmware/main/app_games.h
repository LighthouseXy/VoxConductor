#pragma once
#include "app_button.h"
#include "lvgl.h"
// open/close/tick持LVGL锁；游戏对象只在打开时分配，离开即释放。
void app_games_open(lv_obj_t *parent, unsigned kind);
void app_games_close(void);
// true表示请求退出，由桌面导航负责返回父菜单。
bool app_games_tick(void);
// 按键任务只入固定队列，不等待LVGL。PRESS_DOWN返回false以保留长按。
bool app_games_active(void);
bool app_games_button(app_button_event_t event);
