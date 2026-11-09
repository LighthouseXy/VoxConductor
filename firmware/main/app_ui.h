#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

typedef enum {
  APP_UI_VOICE_LISTENING = 0,
  APP_UI_VOICE_THINKING,
  APP_UI_VOICE_SPEAKING,
  APP_UI_VOICE_ERROR,
  APP_UI_VOICE_REMINDER,
  APP_UI_VOICE_CANCELLED,
} app_ui_voice_state_t;

// 在LVGL初始化完成后创建所有页面、覆盖层和UI定时器。
esp_err_t app_ui_init(void);

// 导航只在LVGL定时器或持锁任务中执行；传感器回调只投递待处理动作。
void app_ui_show_next_page(void);
void app_ui_show_previous_page(void);
// 单击进入当前基础页的二级页面；双击返回原基础页，不切换相邻页。
void app_ui_enter_page(void);
void app_ui_back_page(void);
// 天气详情优先消费左右输入，不让详情浏览落入基础页翻页。
bool app_ui_detail_motion(int direction);
// 持LVGL锁：全局唤醒进入助手，未确认的菜单编辑会撤销预览。
// 失败/已取消覆盖层可重启；正在录音/回答/提醒的覆盖层不可抢占。
bool app_ui_wake_voice(void);
// 持LVGL锁调用；输入恢复基础亮度并唤醒待机，true表示只消费此次唤醒。
bool app_ui_note_activity(void);
// 持LVGL锁请求预览已选主页；timer应用，不打断语音。
void app_ui_preview_home(void);
// 媒体菜单进入相册：倾斜换图、单击显示信息、双击回媒体菜单。
void app_ui_open_album(void);
// 以下入口由LVGL锁保护；录音只允许在语音助手基础页开始。
bool app_ui_voice_allowed(void);
bool app_ui_handle_long_press(void);
// 取走失败页的重试请求；重试录制新语句，不复用已经释放的音频。
bool app_ui_take_retry(void);

// 可从普通FreeRTOS任务调用，内部负责获取LVGL锁。
void app_ui_show_voice_state(app_ui_voice_state_t state, const char *message);
void app_ui_hide_voice_state(void);
// Caller holds LVGL lock. Only explicit bounded destinations are accepted.
bool app_ui_open_destination(const char *target);

// 线程安全的数据入口；UI定时器负责把最新状态绘制到控件。
void app_ui_set_mic_level(uint32_t level);
void app_ui_set_wifi_connected(bool connected);
