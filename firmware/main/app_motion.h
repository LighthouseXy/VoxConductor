#pragma once

#include "esp_err.h"
#include <stdbool.h>

#define APP_MOTION_SDA_GPIO 39
#define APP_MOTION_SCL_GPIO 40
// 新PCB的INT接GPIO41；当前驱动轮询读取，不配置中断。
// 装壳实测：左倾Y为正、右倾Y为负（0=X，1=Y；Z正向朝上）。
#define APP_MOTION_AXIS 1
// 恢复同向控制：左倾向左/上一页，右倾向右/下一页。
#define APP_MOTION_DIRECTION -1
// 上倾向上，下倾向下；页面、菜单和连续滚动共用此映射。
#define APP_MOTION_VERTICAL_DIRECTION -1
// 临时动作诊断，每500ms输出一次；完成实板定位后改为0关闭。
#define APP_MOTION_DIAGNOSTICS 0

// 回调运行在传感器任务；UI调用需加LVGL锁。
typedef void (*app_motion_callback_t)(int direction);
esp_err_t app_motion_init(app_motion_callback_t callback);
void app_motion_set_enabled(bool enabled);
// 最新有效的已校准逻辑屏幕坐标：x右正、y下正；与物理倾斜同向。
// 减去放平静置校准的两轴中位；默认中位为0，不以进入页面时的姿态作为零点。
// 超过200ms无新样本返回false。
bool app_motion_sample(int *x_mg, int *y_mg);
// 只有最近200ms内的有效新样本可用；读取失败立即不可用。
bool app_motion_available(void);
// 请求传感器任务清除两轴校准窗口，放平稳定1秒后重新校准。
// 无有效样本返回false；等待期间不以倾斜姿态作为零点，也不提交翻页。
bool app_motion_request_calibration(void);
bool app_motion_calibrating(void);
