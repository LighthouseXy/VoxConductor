#pragma once
#include "lvgl.h"
#include <stdbool.h>
#include <stdint.h>
// UI函数由LVGL锁保护；service只由与语音互斥的后台协调任务调用。
void app_desktop_init(void);
void app_desktop_create(lv_obj_t *screen);
void app_desktop_visible(bool visible);
// 四宫格分页工具菜单；传感器失效时按键降级模式在进入菜单时固定。
void app_desktop_open_root(void);
// 0番茄钟、1词卡、2任务、3媒体、4测反应、5弹球、6太空飞机。
void app_desktop_open_tool(unsigned index);
void app_desktop_open_settings(void);
// 时钟单击进入闹钟表单；单击编辑/确认字段，双击撤销字段或返回主页。
void app_desktop_open_clock(void);
// 单击(false)执行当前选项，长按(true)菜单消费。
// 传感器失效时菜单单击轮换、长按确认；0=非工具页，1=已消费。
int app_desktop_button(bool long_press);
// 双击返回明确父级；0=非工具页，1=已返回，3=退出工具菜单回基础父页。
int app_desktop_back(void);
// 持LVGL锁查询基础亮度（包括设置页预览），夜间调暗不改此值或NVS。
unsigned app_desktop_base_brightness(void);
// 持LVGL锁读取：0圆形时钟（默认），1壁纸与数字时间。
unsigned app_desktop_home_style(void);
unsigned app_desktop_wallpaper_mode(void);
void app_desktop_select_wallpaper_mode(
    unsigned mode); // Caller holds LVGL lock.
typedef struct {
  unsigned home_style, wallpaper_mode, volume, brightness;
  bool wake_enabled;
  unsigned work_minutes, break_minutes;
  unsigned tts_rate_percent, tts_voice_id;
  uint32_t revision; // 运行期版本，不写入NVS；只用于并发编辑核对。
} app_desktop_config_t;
typedef enum {
  APP_CONFIG_HOME = 1u << 0, APP_CONFIG_WALLPAPER = 1u << 1,
  APP_CONFIG_VOLUME = 1u << 2, APP_CONFIG_BRIGHTNESS = 1u << 3,
  APP_CONFIG_WAKE = 1u << 4, APP_CONFIG_WORK = 1u << 5,
  APP_CONFIG_BREAK = 1u << 6, APP_CONFIG_TTS_RATE = 1u << 7,
  APP_CONFIG_TTS_VOICE = 1u << 8, APP_CONFIG_ALL = (1u << 9) - 1
} app_config_field_t;
typedef enum {
  APP_CONFIG_SAVED, APP_CONFIG_BUSY, APP_CONFIG_INVALID,
  APP_CONFIG_CONFLICT, APP_CONFIG_SAVE_FAILED
} app_config_result_t;
// 只修改mask中的字段，其他设置在LVGL锁内读取并保留。
// expected_revision=0用于兼容旧调用；非0必须与当前快照一致，否则不写入。
// SAVED表示NVS提交成功；SAVE_FAILED表示运行值可能已生效，调用者应回读。
// 和完整setter一样，只由后台协调任务/语音任务互斥调用，不在网络回调使用。
app_config_result_t app_desktop_configuration_patch(
    app_desktop_config_t *out, const app_desktop_config_t *patch,
    unsigned mask, uint32_t expected_revision);
// Backend caller, acquires LVGL lock; setter returns only after NVS commit.
bool app_desktop_configuration(app_desktop_config_t *out,
                               const app_desktop_config_t *set);
bool app_desktop_motion(int direction);
void app_desktop_service(void);
bool app_desktop_take_focus_notice(char title[128]);
void app_desktop_home_text(char text[192]);
// 消费菜单长按的释放事件，atomic结束按键保持状态，调用时不需要LVGL锁。
void app_desktop_button_released(void);
// 语音识别后的明确本机功能指令，非匹配返回NULL，调用者free。
char *app_desktop_voice_command(const char *text);
typedef struct {
  unsigned focus_seconds, work_minutes, break_minutes;
  bool focus_running, focus_resting;
} app_desktop_status_t;
bool app_desktop_status(app_desktop_status_t *out);
// Caller holds LVGL lock; no SD/network/audio waits inside these operations.
bool app_desktop_focus_control(const char *action, unsigned minutes, bool pomodoro);
void app_desktop_open_voice_menu(const char *target);
