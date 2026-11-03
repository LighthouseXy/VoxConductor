/*
 * 桌面工具/设置状态机：菜单层级、编辑草稿、按键/倾斜交互和偏好。
 * UI状态用LVGL锁保护，NVS在锁外写入，由后台协调路径串行执行。
 * 远程修改同步当前编辑值，返回菜单不能把旧草稿覆盖到新设置。
 */
#include "esp_attr.h"
#include "app_desktop.h"
#include "app_agent.h"
#include "app_audio.h"
#include "app_button.h"
#include "app_cloud.h"
#include "app_control.h"
#include "app_display.h"
#include "app_focus.h"
#include "app_words.h"
#include "app_games.h"
#include "app_menu_grid.h"
#include "app_media.h"
#include "app_media_web.h"
#include "app_memory_alloc.h"
#include "app_motion.h"
#include "app_motion_gesture.h"
#include "app_network.h"
#include "app_storage.h"
#include "app_ui.h"
#include "app_ui_theme.h"
#include "app_wake.h"
#include "app_tts_options.h"
#include "esp_lvgl_port.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <math.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
  MENU_ROOT,
  MENU_TASKS,
  MENU_TASK,
  MENU_TASK_ACTION,
  MENU_CONFIRM,
  MENU_FOCUS,
  MENU_WORDS,
  MENU_ARCADE,
  MENU_SETTINGS,
  MENU_VOLUME,
  MENU_STORAGE,
  MENU_BRIGHTNESS,
  MENU_DIRECTION,
  MENU_CALIBRATION,
  MENU_MEDIA,
  MENU_MUSIC,
  MENU_TRACKS,
  MENU_MUSIC_TIMER,
  MENU_WEB,
  MENU_APPEARANCE,
  MENU_WALLPAPER,
  MENU_ANNIVERSARIES,
  MENU_ALARM,
  MENU_WORK_TIME,
  MENU_BREAK_TIME,
  MENU_COUNT
};
typedef struct {
  char id[64], action[16];
  unsigned hour, minute, repeat;
} task_command_t;
// Preserve historical NVS layout, without active game state.
typedef struct {
  unsigned version, volume, reserved_legacy[9];
  bool wake_enabled;
  app_focus_t focus;
} legacy_preferences_t;
typedef struct {
  unsigned version, volume,
      tts_rate_percent, tts_voice_id, reserved_legacy[16];
  bool wake_enabled;
  app_focus_t focus;
  unsigned brightness;
} preferences_t;
// V5仅复用V3/V4预留区的前两个unsigned，不改变后续字段的位置。
// 保留历史布局用于编译期保护；这里只定义类型，不增加运行时状态。
typedef struct {
  unsigned version, volume, reserved_legacy[18];
  bool wake_enabled;
  app_focus_t focus;
  unsigned brightness;
} preferences_v4_layout_t;
_Static_assert(sizeof(preferences_t) == sizeof(preferences_v4_layout_t),
               "NVS preferences blob size must remain compatible");
#define CHECK_PREF_OFFSET(field) \
  _Static_assert(offsetof(preferences_t, field) == \
                 offsetof(preferences_v4_layout_t, field), \
                 "NVS preferences field offset changed: " #field)
CHECK_PREF_OFFSET(version);
CHECK_PREF_OFFSET(volume);
CHECK_PREF_OFFSET(wake_enabled);
CHECK_PREF_OFFSET(focus);
CHECK_PREF_OFFSET(brightness);
#undef CHECK_PREF_OFFSET
static preferences_t s_preferences = {
    .version = 5,
    .volume = 35,
    .tts_rate_percent = 100,
    .brightness = 100,
    .focus = {.work_minutes = 25, .break_minutes = 5}};
static QueueHandle_t s_commands;
static lv_obj_t *s_title, *s_body, *s_hint, *s_value, *s_position;
static lv_obj_t *s_rows[3], *s_tool_icons[3];
static holo_robot_t s_robot;
static lv_obj_t *s_grid, *s_grid_tiles[4], *s_grid_names[4],
    *s_qr;
static lv_obj_t *s_focus_ring;
static lv_obj_t *s_grid_icons[4];
static app_words_t s_words;
static bool s_words_dirty, s_focus_confirm;
static int64_t s_words_retry_at;
static char s_qr_data[96];
LV_FONT_DECLARE(app_ui_font_14);
static motion_gesture_t s_grid_vertical;
static EXT_RAM_BSS_ATTR app_task_view_t s_tasks[8];
static unsigned s_count, s_edit_value, s_journal_bytes, s_archives;
static int s_menu = MENU_ROOT, s_selection;
static int s_selections[MENU_COUNT];
static int64_t s_last_frame, s_last_save, s_last_menu_frame, s_feedback_until;
static int64_t s_capacity_at, s_rendered_second = -1;
static bool s_visible, s_dirty, s_focus_notice, s_tasks_ready, s_storage_ready;
static bool s_render_dirty = true, s_rendered_sensor;
static bool s_calibration_waiting, s_calibration_finished,
    s_preferences_writable = true;
// Lock the emergency button-only mode for this menu session. A stale sensor
// sample must not swap the meaning of short/long presses halfway through.
static bool s_button_navigation;
static char s_feedback[128], s_notice_title[128], s_task_pending_id[64];
static _Atomic bool s_wait_release;
static char s_task_outcome[128];
static bool s_task_result_waiting;
static unsigned s_alarm_hour, s_alarm_minute, s_alarm_repeat;
static bool s_alarm_editing;
static motion_gesture_t s_alarm_vertical;
static int s_alarm_field = -1;
static int32_t s_alarm_fraction;
static bool s_alarm_scroll_armed;
static uint32_t s_alarm_center_since;
#define ALARM_DEADZONE_MG 120
#define ALARM_FULL_TILT_MG 600
#define ALARM_MIN_SPEED 1
#define ALARM_HOUR_MAX_SPEED 6
#define ALARM_MINUTE_MAX_SPEED 20
static const char *alarm_repeats[] = {"once", "daily", "weekdays"};
static const char *alarm_repeat_labels[] = {"仅一次", "每天", "工作日"};
// Catalogs stay in static storage: the 32-file snapshot is too large for a UI
// stack.
static EXT_RAM_BSS_ATTR app_media_snapshot_t s_media;
static app_media_appearance_t s_appearance;
static app_media_web_status_t s_web;
static app_media_anniversary_t s_anniversaries[APP_MEDIA_ANNIVERSARIES];
static bool s_ambient_browser, s_wallpaper_from_media;
static unsigned s_home_style;
static unsigned s_wallpaper_mode; // 0 SD image, 1..4 procedural wallpaper.
static const char *home_style_names[] = {"圆形时钟", "壁纸+数字时间"};
enum { WALLPAPER_DYNAMIC_FIRST = 2, WALLPAPER_IMAGE_FIRST = 6 };
static const char *wallpaper_names[] = {"星空闪烁", "轨道星环", "漂浮水母",
                                        "光点花园"};
static const char *tool_items[] = {"番茄钟", "背单词", "任务", "媒体",
                                  "测反应", "弹球", "太空飞机"};
static lv_obj_t *s_desktop_parent;
static const char *media_items[] = {"相册", "音乐", "环境音", "壁纸",
                                    "网页管理"};
static const char *music_items[] = {"选择曲目", "播放 / 暂停", "上一首",
                                    "下一首",   "停止",        "单曲循环",
                                    "睡眠定时"};
static const char *appearance_items[] = {"圆形时钟", "壁纸+数字时间",
                                         "选择壁纸", "夜间调暗", "闲置待机"};
static const unsigned music_minutes[] = {0, 15, 30, 60};
static const char *settings_items[] = {"显示亮度", "声音音量", "主页外观",
                                       "专注时长", "休息时长", "显示方向",
                                       "倾斜校准", "存储状态"};
static bool menu_move(int direction);

static void enter(int menu) {
  if (s_menu == MENU_ARCADE && menu != MENU_ARCADE) app_games_close();
  s_selections[s_menu] = s_selection;
  s_menu = menu;
  s_selection = s_selections[menu];
  s_grid_vertical = (motion_gesture_t){.require_stable_center = true};
  s_render_dirty = true;
}
static void feedback(const char *message) {
  snprintf(s_feedback, sizeof(s_feedback), "%s", message);
  s_feedback_until = esp_timer_get_time() + 4000000;
  s_render_dirty = true;
}
static void set_text(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text))
    lv_label_set_text(label, text);
}
static void show(lv_obj_t *object, bool visible) {
  if (visible)
    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static void save_later(void) {
  s_dirty = true;
  s_render_dirty = true;
}
static void select_home_style(unsigned style) {
  if (style > 1)
    return;
  // Change the home only; keep the chosen wallpaper for the next switch back.
  s_home_style = style;
  save_later();
  app_ui_preview_home();
}
static void open_wallpapers(bool from_media) {
  s_wallpaper_from_media = from_media;
  enter(MENU_WALLPAPER);
  // Always reveal both home choices, even with a long SD wallpaper catalog.
  s_selection = (int)s_home_style;
}
static const char *wallpaper_item(unsigned index) {
  if (index < WALLPAPER_DYNAMIC_FIRST)
    return home_style_names[index];
  if (index < WALLPAPER_IMAGE_FIRST)
    return wallpaper_names[index - WALLPAPER_DYNAMIC_FIRST];
  unsigned image = index - WALLPAPER_IMAGE_FIRST;
  return image < s_media.image_count
             ? app_media_file_label(s_media.images[image]) : "暂无图片";
}
void app_desktop_button_released(void) { atomic_store(&s_wait_release, false); }
static unsigned audio_count(bool ambient) {
  unsigned count = 0;
  for (unsigned i = 0; i < s_media.audio_count; ++i)
    count += app_media_audio_is_ambient(s_media.audio[i]) == ambient;
  return count;
}
static unsigned audio_index(unsigned selection) {
  for (unsigned i = 0; i < s_media.audio_count; ++i)
    if (app_media_audio_is_ambient(s_media.audio[i]) == s_ambient_browser) {
      if (!selection)
        return i;
      --selection;
    }
  return APP_MEDIA_MAX_FILES;
}
static bool audio_current(void) {
  return s_media.audio_index < s_media.audio_count &&
         app_media_audio_is_ambient(s_media.audio[s_media.audio_index]) ==
             s_ambient_browser;
}
static unsigned audio_selection(void) {
  unsigned selection = 0;
  if (!audio_current())
    return 0;
  for (unsigned i = 0; i < s_media.audio_index; ++i)
    selection +=
        app_media_audio_is_ambient(s_media.audio[i]) == s_ambient_browser;
  return selection;
}
static unsigned menu_count(void) {
  switch (s_menu) {
  case MENU_ROOT:
    return sizeof(tool_items) / sizeof(*tool_items);
  case MENU_ALARM:
    return 4;
  case MENU_MEDIA:
    return sizeof(media_items) / sizeof(*media_items);
  case MENU_MUSIC:
    return sizeof(music_items) / sizeof(*music_items);
  case MENU_TRACKS:
    return audio_count(s_ambient_browser) ? audio_count(s_ambient_browser) : 1;
  case MENU_WALLPAPER:
    return s_media.image_count + WALLPAPER_IMAGE_FIRST;
  case MENU_MUSIC_TIMER:
    return sizeof(music_minutes) / sizeof(*music_minutes);
  case MENU_APPEARANCE:
    return sizeof(appearance_items) / sizeof(*appearance_items);
  case MENU_ANNIVERSARIES:
    return APP_MEDIA_ANNIVERSARIES;
  case MENU_TASKS:
    return s_count ? s_count : 1;
  case MENU_SETTINGS:
    return sizeof(settings_items) / sizeof(*settings_items);
  case MENU_FOCUS:
    return 2;
  case MENU_WORDS:
    return s_words.revealed && app_words_current(&s_words) ? 2 : 1;
  default:
    return 1;
  }
}
static const char *status_label(const char *status) {
  if (!strcmp(status, "pending"))
    return "待完成";
  if (!strcmp(status, "completed"))
    return "已完成";
  if (!strcmp(status, "played"))
    return "已提醒";
  if (!strcmp(status, "cancelled"))
    return "已取消";
  if (!strcmp(status, "failed"))
    return "提醒失败";
  if (!strcmp(status, "missed"))
    return "已错过";
  return "已提交";
}
static void format_due(char out[32], long long due) {
  if (!due) {
    strcpy(out, "无截止时间");
    return;
  }
  time_t local = (time_t)(due + 28800);
  struct tm tm;
  gmtime_r(&local, &tm);
  strftime(out, 32, "%m-%d %H:%M", &tm);
}
static void short_title(char out[64], const char *text, unsigned limit) {
  unsigned count = 0;
  const unsigned char *p = (const unsigned char *)text;
  while (*p && count < limit) {
    unsigned bytes = *p < 0x80 ? 1 : *p < 0xE0 ? 2 : *p < 0xF0 ? 3 : 4;
    for (unsigned i = 0; i < bytes; i++)
      if (!p[i]) {
        bytes = i;
        break;
      }
    if ((const char *)p - text + bytes > 60)
      break;
    p += bytes;
    count++;
  }
  size_t bytes = (const char *)p - text;
  memcpy(out, text, bytes);
  out[bytes] = 0;
  for (size_t i = 0; i < bytes; i++)
    if (out[i] == '\n' || out[i] == '\r')
      out[i] = ' ';
  if (*p)
    strcat(out, "...");
}
static bool grid_menu(void) {
  return s_menu == MENU_ROOT ||
         s_menu == MENU_SETTINGS ||
         s_menu == MENU_MEDIA || s_menu == MENU_MUSIC ||
         s_menu == MENU_APPEARANCE || s_menu == MENU_WALLPAPER ||
         s_menu == MENU_TRACKS || s_menu == MENU_TASKS ||
         s_menu == MENU_MUSIC_TIMER || s_menu == MENU_ANNIVERSARIES;
}
static const char *grid_name(unsigned index, char text[64]) {
  const char *name = "";
  switch (s_menu) {
  case MENU_ROOT:
    name = tool_items[index];
    break;
  case MENU_SETTINGS:
    name = settings_items[index];
    break;
  case MENU_MEDIA:
    name = media_items[index];
    break;
  case MENU_MUSIC:
    name = music_items[index];
    break;
  case MENU_APPEARANCE:
    name = appearance_items[index];
    break;
  case MENU_WALLPAPER:
    name = wallpaper_item(index);
    break;
  case MENU_TRACKS: {
    unsigned at = audio_index(index);
    name = at < s_media.audio_count ? app_media_file_label(s_media.audio[at])
                                    : "暂无音乐";
    break;
  }
  case MENU_TASKS:
    name = index < s_count ? s_tasks[index].title : "暂无任务";
    break;
  case MENU_MUSIC_TIMER:
    snprintf(text, 64, music_minutes[index] ? "%u分钟" : "关闭",
             music_minutes[index]);
    return text;
  case MENU_ANNIVERSARIES:
    name = s_anniversaries[index].label[0] ? s_anniversaries[index].label
                                           : "未设置";
    break;
  default:
    break;
  }
  short_title(text, name, 8);
  return text;
}
static bool render_grid(void) {
  if (!grid_menu())
    return false;
  unsigned count = menu_count(), start = (unsigned)s_selection / 4 * 4;
  // 每页固定四格，复用现有对象；空格不响应，不在页外放说明文字。
  for (unsigned i = 0; i < 4; i++) {
    unsigned at = start + i;
    bool valid = at < count;
    show(s_grid_tiles[i], valid);
    if (!valid)
      continue;
    // Alarm editing reuses these cells; restore the ordinary grid geometry.
    lv_obj_set_pos(s_grid_tiles[i], (int)(i % 2) * 116, (int)(i / 2) * 116);
    lv_obj_set_size(s_grid_tiles[i], 108, 108);
    lv_obj_set_pos(s_grid_names[i], 6, 62);
    lv_obj_set_size(s_grid_names[i], 96, 42);
    lv_obj_set_style_text_font(s_grid_names[i], &app_ui_font_20, 0);
    lv_obj_set_style_border_color(s_grid_tiles[i], lv_color_hex(HOLO_APRICOT), 0);
    char name[64];
    set_text(s_grid_names[i], grid_name(at, name));
    bool selected = at == (unsigned)s_selection;
    static const char *tool_symbols[] = {LV_SYMBOL_LOOP, LV_SYMBOL_EDIT, LV_SYMBOL_LIST, LV_SYMBOL_AUDIO,
                                        LV_SYMBOL_BELL, LV_SYMBOL_SHUFFLE, LV_SYMBOL_UP};
    static const char *setting_symbols[] = {LV_SYMBOL_EYE_OPEN, LV_SYMBOL_AUDIO, LV_SYMBOL_IMAGE, LV_SYMBOL_LOOP,
                                            LV_SYMBOL_PAUSE, LV_SYMBOL_REFRESH, LV_SYMBOL_SETTINGS, LV_SYMBOL_SD_CARD};
    set_text(s_grid_icons[i], s_menu == MENU_ROOT ? tool_symbols[at] :
             s_menu == MENU_SETTINGS ? setting_symbols[at] : LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(s_grid_icons[i], lv_color_hex(selected ? HOLO_APRICOT : HOLO_MINT), 0);
    show(s_grid_icons[i], !s_feedback[0] || !selected);
    if (selected && s_feedback[0]) {
      set_text(s_grid_names[i], s_feedback);
      lv_obj_set_pos(s_grid_names[i], 6, 12);
      lv_obj_set_height(s_grid_names[i], 84);
      lv_obj_set_style_text_font(s_grid_names[i], &app_ui_font_16, 0);
    }
    lv_obj_set_style_border_width(s_grid_tiles[i], selected ? 4 : 2, 0);
    lv_obj_set_style_border_opa(s_grid_tiles[i],
                                selected ? LV_OPA_COVER : LV_OPA_80, 0);
    lv_obj_set_style_text_color(
        s_grid_names[i], lv_color_hex(selected ? HOLO_APRICOT : HOLO_CREAM), 0);
  }
  lv_obj_set_pos(s_grid, 8, 8);
  lv_obj_set_size(s_grid, 224, 224);
  show(s_grid, true);
  return true;
}
static void render_alarm(void) {
  // 复用四个菜单格和已有标签，不再创建另一套闹钟对象。
  lv_obj_set_pos(s_grid, 8, 50);
  lv_obj_set_size(s_grid, 224, 164);
  for (unsigned i = 0; i < 4; ++i) {
    show(s_grid_tiles[i], true);
    bool time_field = i < 2;
    lv_obj_set_pos(s_grid_tiles[i], time_field ? 12 + (int)i * 104 : 12,
                    time_field ? 0 : i == 2 ? 90 : 132);
    lv_obj_set_size(s_grid_tiles[i], time_field ? 96 : 200,
                     time_field ? 80 : 32);
    lv_obj_set_pos(s_grid_names[i], 3, time_field ? 5 : 7);
    lv_obj_set_size(s_grid_names[i], time_field ? 90 : 194, 17);
    lv_obj_set_style_text_font(s_grid_names[i], &app_ui_font_14, 0);
    bool selected = (int)i == s_selection;
    unsigned color = selected && s_alarm_editing ? HOLO_MINT : HOLO_APRICOT;
    lv_obj_set_style_border_color(s_grid_tiles[i], lv_color_hex(color), 0);
    lv_obj_set_style_border_width(s_grid_tiles[i], selected ? 2 : 1, 0);
    lv_obj_set_style_border_opa(s_grid_tiles[i],
                                selected ? LV_OPA_COVER : LV_OPA_40, 0);
    lv_obj_set_style_text_color(s_grid_names[i],
                                lv_color_hex(selected ? color : HOLO_CREAM), 0);
    char name[64];
    if (i < 2) snprintf(name, sizeof(name), "%s", i ? "分钟" : "小时");
    else if (i == 2) snprintf(name, sizeof(name), "重复：%s",
                              alarm_repeat_labels[s_alarm_repeat]);
    else snprintf(name, sizeof(name), "%s",
                   s_task_pending_id[0] ? "正在保存" : "保存闹钟");
    set_text(s_grid_names[i], name);
  }
  for (unsigned i = 0; i < 2; ++i) {
    char digits[12];
    snprintf(digits, sizeof(digits), "%02u", i ? s_alarm_minute : s_alarm_hour);
    set_text(s_rows[i], digits);
    lv_obj_set_pos(s_rows[i], 24 + (int)i * 104, 82);
    lv_obj_set_size(s_rows[i], 88, 35);
    lv_obj_set_style_text_font(s_rows[i], &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_color(s_rows[i],
        lv_color_hex((int)i == s_selection && s_alarm_editing ? HOLO_MINT : HOLO_CREAM), 0);
    show(s_rows[i], true);
    lv_obj_move_foreground(s_rows[i]);
  }
  show(s_body, false);
  show(s_value, false);
  show(s_grid, true);
}
// 工具页用同一组对象显示明确的动作，不把“暂停”和“退出”藏进长按。
static void tool_actions(const char *first, const char *second) {
  lv_obj_set_pos(s_grid, 8, 196);
  lv_obj_set_size(s_grid, 224, 36);
  for (unsigned i = 0; i < 4; ++i) {
    show(s_grid_tiles[i], i == 0 || (i == 1 && second));
    if (i > 1 || (i == 1 && !second)) continue;
    int width = second ? 108 : 224;
    lv_obj_set_pos(s_grid_tiles[i], (int)i * 116, 0);
    lv_obj_set_size(s_grid_tiles[i], width, 36);
    lv_obj_set_style_border_width(s_grid_tiles[i], s_selection == (int)i ? 3 : 1, 0);
    lv_obj_set_style_border_opa(s_grid_tiles[i], LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_grid_tiles[i], lv_color_hex(HOLO_APRICOT), 0);
    lv_obj_set_pos(s_grid_names[i], 3, 8);
    lv_obj_set_size(s_grid_names[i], width - 6, 22);
    lv_obj_set_style_text_font(s_grid_names[i], &app_ui_font_16, 0);
    lv_obj_set_style_text_color(s_grid_names[i], lv_color_hex(s_selection == (int)i ? HOLO_APRICOT : HOLO_CREAM), 0);
    set_text(s_grid_names[i], i ? second : first);
  }
  show(s_grid, true);
}
static void render_menu(void) {
  if (s_menu == MENU_ARCADE) return;
  int64_t now = esp_timer_get_time();
  bool sensor = app_motion_available();
  if (sensor != s_rendered_sensor) {
    s_rendered_sensor = sensor;
    s_render_dirty = true;
  }
  if (s_feedback[0] && now >= s_feedback_until) {
    s_feedback[0] = 0;
    s_render_dirty = true;
  }
  bool media_menu = s_menu >= MENU_MEDIA && s_menu <= MENU_ANNIVERSARIES;
  bool live = s_menu == MENU_FOCUS || s_menu == MENU_STORAGE ||
              s_menu == MENU_CALIBRATION || media_menu;
  if (!s_render_dirty && (!live || now / 1000000 == s_rendered_second))
    return;
  s_rendered_second = now / 1000000;
  if (media_menu) {
    app_media_snapshot(&s_media);
    app_media_get_appearance(&s_appearance);
    app_media_web_status(&s_web);
    if (s_menu == MENU_ANNIVERSARIES)
      app_media_get_anniversaries(s_anniversaries);
  }
  unsigned count = menu_count();
  if (!count || s_selection < 0 || (unsigned)s_selection >= count)
    s_selection = s_menu == MENU_ROOT && count ? (int)count - 1 : 0;
  char title[128] = {0}, body[512] = {0}, value[64] = {0}, position[48] = {0};
  const char *hint = "";
  show(s_grid, false);
  show(s_focus_ring, false);
  for (unsigned i = 0; i < 4; ++i) show(s_grid_icons[i], false);
  show(s_qr, false);
  show(s_robot.group, false);
  for (unsigned i = 0; i < 3; i++) {
    lv_obj_set_pos(s_rows[i], 20, 96 + (int)i * 33);
    lv_obj_set_width(s_rows[i], 200);
    lv_obj_set_height(s_rows[i], 24);
    lv_obj_set_style_text_font(s_rows[i], &app_ui_font_20, 0);
    show(s_rows[i], false);
    show(s_tool_icons[i], false);
  }
  lv_obj_set_pos(s_body, 20, 134);
  lv_obj_set_size(s_body, 200, 58);
  lv_obj_set_style_text_font(s_body, &app_ui_font_16, 0);
  lv_obj_set_style_text_line_space(s_body, 5, 0);
  lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_set_pos(s_value, 20, 65);
  lv_obj_set_size(s_value, 200, 54);
  lv_obj_set_style_text_font(s_value, &lv_font_montserrat_48, 0);
  switch (s_menu) {
  case MENU_ALARM: {
    static const char *fields[] = {"调整小时", "调整分钟", "重复方式"};
    snprintf(title, sizeof(title), "%s",
             s_alarm_editing ? fields[s_selection] : "设置闹钟");
    break;
  }
  case MENU_ROOT:
    snprintf(title, sizeof(title), "桌面工具");
    if (s_selection < 3)
      show(s_tool_icons[s_selection], true);
    else {
      show(s_robot.group, true);
      holo_robot_pose(&s_robot, 0, false);
    }
    snprintf(body, sizeof(body), "%s", tool_items[s_selection]);
    lv_obj_set_style_text_font(s_body, &app_ui_font_20, 0);
    snprintf(position, sizeof(position), "%d/%u", s_selection + 1, count);
    break;
  case MENU_FOCUS: {
    app_focus_t *f = &s_preferences.focus;
    unsigned remain = app_focus_remaining(f, now);
    if (!remain)
      remain = f->work_minutes * 60;
    snprintf(title, sizeof(title), "%s", s_focus_confirm ? "结束本轮？" : f->resting ? "休息一下" : "番茄钟");
    snprintf(value, sizeof(value), "%02u:%02u", remain / 60, remain % 60);
    lv_obj_set_y(s_value, 83);
    lv_obj_set_pos(s_body, 42, 139);
    lv_obj_set_size(s_body, 156, 44);
    snprintf(body, sizeof(body), "%s\n已完成 %u 轮", f->running ? "进行中" :
             f->interrupted ? "重启后已暂停" : f->remaining ? "已暂停" : "准备开始", f->cycles);
    unsigned total = (f->resting ? f->break_minutes : f->work_minutes) * 60;
    lv_arc_set_value(s_focus_ring, total && remain < total ? (int)((total - remain) * 100 / total) : 0);
    show(s_focus_ring, true);
    break;
  }
  case MENU_WORDS: {
    const app_word_t *word = app_words_current(&s_words);
    lv_obj_set_style_text_font(s_value, &lv_font_montserrat_32, 0);
    if (!word) {
      snprintf(title, sizeof(title), "本组完成");
      snprintf(value, sizeof(value), "%u / %u", app_words_mastered(&s_words), APP_WORD_COUNT);
      snprintf(body, sizeof(body), "已掌握词卡\n再来一组，继续积累");
    } else {
      snprintf(title, sizeof(title), "词卡 %u / %u", s_words.at + 1, s_words.count);
      snprintf(value, sizeof(value), "%s", word->word);
      lv_obj_set_y(s_value, 59);
      lv_obj_set_pos(s_body, 16, 108);
      lv_obj_set_size(s_body, 208, 42);
      lv_obj_set_style_text_font(s_body, &app_ui_font_20, 0);
      snprintf(body, sizeof(body), "%s", s_words.revealed ? word->meaning : "先回想，再翻卡");
      if (s_words.revealed) {
        set_text(s_rows[0], word->example);
        lv_obj_set_pos(s_rows[0], 16, 152);
        lv_obj_set_size(s_rows[0], 208, 38);
        lv_obj_set_style_text_font(s_rows[0], &app_ui_font_16, 0);
        lv_label_set_long_mode(s_rows[0], LV_LABEL_LONG_MODE_WRAP);
        show(s_rows[0], true);
      }
    }
    break;
  }
  case MENU_TASKS:
  case MENU_TASK:
    snprintf(title, sizeof(title), "任务");
    if (!s_tasks_ready)
      snprintf(body, sizeof(body), "正在读取任务");
    else if (!s_count)
      snprintf(body, sizeof(body), "暂无任务");
    else {
      unsigned index = s_menu == MENU_TASK ? (unsigned)s_selections[MENU_TASKS]
                                           : (unsigned)s_selection;
      if (index >= s_count)
        index = 0;
      const app_task_view_t *task = &s_tasks[index];
      bool memo = !strcmp(task->kind, "note");
      if (memo)
        snprintf(title, sizeof(title), "备忘录");
      char due[32];
      format_due(due, task->due_utc);
      char name[64];
      short_title(name, task->title, 16);
      snprintf(body, sizeof(body), "%s", memo ? task->title : name);
      lv_obj_set_pos(s_body, 20, 65);
      lv_obj_set_height(s_body, memo ? 105 : 62);
      lv_obj_set_style_text_font(s_body,
                                 memo ? &app_ui_font_16 : &app_ui_font_20, 0);
      if (memo)
        lv_obj_set_style_text_line_space(s_body, 0, 0);
      if (!memo)
        snprintf(value, sizeof(value), "%s", due);
      lv_obj_set_pos(s_value, 20, 137);
      lv_obj_set_height(s_value, 25);
      lv_obj_set_style_text_font(s_value, &app_ui_font_16, 0);
      snprintf(position, sizeof(position), "%s  %d/%u",
               memo && !strcmp(task->status, "pending")
                   ? "已记录"
                   : status_label(task->status),
               s_selection + 1, s_count);
      hint = "";
    }
    break;
  case MENU_SETTINGS:
    break;
  case MENU_MEDIA:
    snprintf(title, sizeof(title), "媒体");
    show(s_robot.group, true);
    snprintf(body, sizeof(body), "%s", media_items[s_selection]);
    lv_obj_set_style_text_font(s_body, &app_ui_font_20, 0);
    snprintf(position, sizeof(position), "%u张 / %u首 / %u种",
             s_media.image_count, audio_count(false), audio_count(true));
    break;
  case MENU_MUSIC: {
    snprintf(title, sizeof(title), "%s · %s",
             s_ambient_browser ? "环境音" : "音乐",
             s_ambient_browser && !s_selection ? "选择声音"
                                               : music_items[s_selection]);
    char name[64];
    unsigned selected = audio_current() ? s_media.audio_index : audio_index(0);
    if (selected < s_media.audio_count)
      short_title(name, app_media_file_label(s_media.audio[selected]), 16);
    else
      snprintf(name, sizeof(name), "%s",
               s_ambient_browser ? "暂无环境音" : "暂无本地音乐");
    char state[64];
    short_title(state,
                !audio_current()     ? "尚未播放"
                : s_media.playing    ? "正在播放"
                : s_media.paused     ? "已暂停"
                : s_media.message[0] ? s_media.message
                                     : "已停止",
                20);
    snprintf(body, sizeof(body), "%s\n%s", name, state);
    lv_obj_set_pos(s_body, 20, 64);
    lv_obj_set_height(s_body, 68);
    lv_obj_set_style_text_line_space(s_body, 0, 0);
    unsigned elapsed = audio_current() ? s_media.elapsed_seconds : 0;
    unsigned duration = audio_current() ? s_media.duration_seconds : 0;
    snprintf(value, sizeof(value), "%02u:%02u / %02u:%02u", elapsed / 60,
             elapsed % 60, duration / 60, duration % 60);
    lv_obj_set_pos(s_value, 20, 139);
    lv_obj_set_height(s_value, 25);
    lv_obj_set_style_text_font(s_value, &app_ui_font_16, 0);
    if (s_selection == 5)
      snprintf(position, sizeof(position), "%s",
               s_media.repeat ? "循环开启" : "循环关闭");
    else if (s_media.stop_remaining_seconds)
      snprintf(position, sizeof(position), "定时 %u分钟",
               (s_media.stop_remaining_seconds + 59) / 60);
    else
      snprintf(position, sizeof(position), "%d/%u", s_selection + 1, count);
    break;
  }
  case MENU_TRACKS:
  case MENU_WALLPAPER: {
    bool image = s_menu == MENU_WALLPAPER;
    snprintf(title, sizeof(title), "%s",
             image               ? "选择壁纸"
             : s_ambient_browser ? "选择环境音"
                                 : "选择曲目");
    unsigned files = image ? menu_count() : audio_count(s_ambient_browser);
    char name[64];
    if (files)
      short_title(name, image ? wallpaper_item((unsigned)s_selection)
                             : app_media_file_label(s_media.audio[
                                   audio_index((unsigned)s_selection)]), 18);
    else
      snprintf(name, sizeof(name), "%s",
               s_media.ready ? "请用网页上传文件" : "SD卡不可用");
    snprintf(body, sizeof(body), "%s", name);
    lv_obj_set_pos(s_body, 20, 72);
    lv_obj_set_height(s_body, 90);
    if (image)
      snprintf(position, sizeof(position), "%d/%u · %s", s_selection + 1,
               files, home_style_names[s_home_style]);
    else
      snprintf(position, sizeof(position), "%d/%u", files ? s_selection + 1 : 0,
               files);
    break;
  }
  case MENU_MUSIC_TIMER:
    snprintf(title, sizeof(title), "睡眠定时");
    if (music_minutes[s_selection])
      snprintf(value, sizeof(value), "%u", music_minutes[s_selection]);
    snprintf(body, sizeof(body), "%s",
             music_minutes[s_selection] ? "分钟后停止播放" : "关闭定时");
    break;
  case MENU_WEB:
    snprintf(title, sizeof(title), "网页管理");
    if (s_web.enabled) {
      char data[96];
      snprintf(data, sizeof(data), "%s", s_web.url);
      if (strcmp(data, s_qr_data)) {
        if (lv_qrcode_update(s_qr, data, (uint32_t)strlen(data)) ==
            LV_RESULT_OK)
          snprintf(s_qr_data, sizeof(s_qr_data), "%s", data);
      }
      show(s_qr, true);
      snprintf(body, sizeof(body), "%s", s_web.url);
      lv_obj_set_pos(s_body, 5, 190);
      lv_obj_set_size(s_body, 230, 36);
      lv_obj_set_style_text_font(s_body, &app_ui_font_14, 0);
      lv_obj_set_style_text_line_space(s_body, 0, 0);
    } else {
      snprintf(body, sizeof(body),
               "同一Wi-Fi扫码管理\n媒体、任务与对话记录\n%s", s_web.message);
      lv_obj_set_pos(s_body, 15, 64);
      lv_obj_set_size(s_body, 210, 130);
    }
    break;
  case MENU_APPEARANCE:
    snprintf(title, sizeof(title), "外观与待机");
    snprintf(body, sizeof(body), "%s", appearance_items[s_selection]);
    lv_obj_set_pos(s_body, 20, 76);
    lv_obj_set_style_text_font(s_body, &app_ui_font_20, 0);
    if (s_selection < 2)
      snprintf(position, sizeof(position), "当前 %s",
               home_style_names[s_home_style]);
    else if (s_selection == 3) {
      if (s_appearance.night_enabled)
        snprintf(position, sizeof(position), "%02u:%02u-%02u:%02u / %u%%",
                 s_appearance.night_start_minute / 60,
                 s_appearance.night_start_minute % 60,
                 s_appearance.night_end_minute / 60,
                 s_appearance.night_end_minute % 60,
                 s_appearance.night_brightness);
      else
        snprintf(position, sizeof(position), "已关闭");
    } else if (s_selection == 4) {
      if (s_appearance.idle_seconds)
        snprintf(position, sizeof(position), "%u秒进入待机",
                 s_appearance.idle_seconds);
      else
        snprintf(position, sizeof(position), "已关闭");
    } else
      snprintf(position, sizeof(position), "%u张图片", s_media.image_count);
    break;
  case MENU_ANNIVERSARIES: {
    snprintf(title, sizeof(title), "纪念日 %d/%u", s_selection + 1,
             APP_MEDIA_ANNIVERSARIES);
    const app_media_anniversary_t *item = &s_anniversaries[s_selection];
    app_network_status_t network;
    app_network_status_snapshot(&network);
    int days;
    if (!item->label[0])
      snprintf(body, sizeof(body),
               "尚未设置\n媒体 → 网页管理\n可设置四个纪念日");
    else {
      char name[64];
      short_title(name, item->label, 16);
      snprintf(body, sizeof(body), "%s\n%04u-%02u-%02u\n%s", name, item->year,
               item->month, item->day, item->annual ? "每年纪念" : "单次纪念");
      if (network.time_valid &&
          app_media_anniversary_days(item, (long long)time(NULL), &days)) {
        if (!days)
          snprintf(value, sizeof(value), "就是今天");
        else if (days > 0)
          snprintf(value, sizeof(value), "还有 %d 天", days);
        else
          snprintf(value, sizeof(value), "已过 %d 天", -days);
      } else
        snprintf(value, sizeof(value), "等待校时");
      lv_obj_set_pos(s_value, 20, 146);
      lv_obj_set_height(s_value, 25);
      lv_obj_set_style_text_font(s_value, &app_ui_font_20, 0);
    }
    lv_obj_set_pos(s_body, 20, 57);
    lv_obj_set_height(s_body, 82);
    lv_obj_set_style_text_line_space(s_body, 1, 0);
    hint = "";
    break;
  }
  case MENU_VOLUME:
  case MENU_BRIGHTNESS:
  case MENU_WORK_TIME:
  case MENU_BREAK_TIME:
    snprintf(title, sizeof(title), "%s",
             s_menu == MENU_VOLUME ? "声音音量" :
             s_menu == MENU_BRIGHTNESS ? "显示亮度" :
             s_menu == MENU_WORK_TIME ? "专注时长" : "休息时长");
    snprintf(value, sizeof(value),
             s_menu == MENU_VOLUME || s_menu == MENU_BRIGHTNESS ? "%u%%" : "%u",
             s_edit_value);
    snprintf(body, sizeof(body), "%s",
             !s_button_navigation && sensor ? "左减 / 右增" : "调整数值");
    if (s_menu == MENU_WORK_TIME || s_menu == MENU_BREAK_TIME)
      snprintf(position, sizeof(position), "分钟");
    hint = "";
    break;
  case MENU_DIRECTION:
    snprintf(title, sizeof(title), "显示方向校验");
    snprintf(value, sizeof(value), "F / R");
    lv_obj_set_style_text_font(s_value, &lv_font_montserrat_32, 0);
    snprintf(body, sizeof(body), "左 → 右\n12:34\n经棱镜观察正常方向");
    hint = "";
    break;
  case MENU_CALIBRATION:
    snprintf(title, sizeof(title), "倾斜校准");
    if (s_calibration_waiting && sensor && !app_motion_calibrating()) {
      s_calibration_waiting = false;
      s_calibration_finished = true;
    }
    snprintf(body, sizeof(body), "%s",
             !sensor                    ? "传感器不可用"
             : app_motion_calibrating() ? "放平静置一秒\n正在校准"
             : s_calibration_finished   ? "校准完成\n左右倾斜检查方向"
                                        : "准备校准");
    hint = "";
    break;
  case MENU_STORAGE:
    snprintf(title, sizeof(title), "存储状态");
    snprintf(body, sizeof(body),
             "%s\n%u KiB / 8192 KiB\n归档 %u/4\n电脑可备份SD卡",
             s_storage_ready ? "SD卡已挂载" : "SD卡不可用",
             s_journal_bytes / 1024, s_archives);
    lv_obj_set_pos(s_body, 20, 65);
    lv_obj_set_height(s_body, 116);
    hint = "";
    break;
  default:
    break;
  }
  set_text(s_title, title);
  show(s_title, title[0]);
  set_text(s_body, body);
  show(s_body, body[0]);
  set_text(s_value, value);
  show(s_value, value[0]);
  set_text(s_position, position);
  show(s_position, position[0] && !s_feedback[0]);
  (void)hint;
  set_text(s_hint, s_feedback);
  show(s_hint, s_feedback[0] != 0 && !(s_menu == MENU_WEB && s_web.enabled));
  if (s_menu == MENU_ALARM)
    render_alarm();
  else if (render_grid()) {
    show(s_body, false);
    show(s_value, false);
    show(s_title, false);
    show(s_position, false);
    show(s_hint, false); // 临时反馈显示在选中的卡片内，不占顶部/底部。
    show(s_robot.group, false);
    for (unsigned i = 0; i < 3; i++) {
      show(s_rows[i], false);
      show(s_tool_icons[i], false);
    }
  }
  if (s_menu == MENU_FOCUS)
    tool_actions(s_focus_confirm ? "继续计时" : s_preferences.focus.running ? "暂停" : s_preferences.focus.remaining ? "继续" : "开始", s_focus_confirm ? "确认结束" : "结束");
  else if (s_menu == MENU_WORDS)
    tool_actions(!app_words_current(&s_words) ? "下一组" : s_words.revealed ? "记住了" : "查看释义",
                 app_words_current(&s_words) && s_words.revealed ? "再练一次" : NULL);
  if ((s_menu == MENU_FOCUS || s_menu == MENU_WORDS) && s_feedback[0]) {
    set_text(s_title, s_feedback);
    show(s_title, true);
    show(s_hint, false); // 不让保存反馈覆盖底部操作按钮。
  }
  s_render_dirty = false;
}
// Same bounded, fractional velocity integration as the weather timeline.
// Hours and minutes have different ceilings so both can be set precisely.
static int alarm_speed(int tilt, int maximum) {
  int magnitude = abs(tilt);
  if (magnitude <= ALARM_DEADZONE_MG) return 0;
  if (magnitude > ALARM_FULL_TILT_MG) magnitude = ALARM_FULL_TILT_MG;
  int speed = ALARM_MIN_SPEED +
      (magnitude - ALARM_DEADZONE_MG) * (maximum - ALARM_MIN_SPEED) /
      (ALARM_FULL_TILT_MG - ALARM_DEADZONE_MG);
  return tilt < 0 ? -speed : speed;
}
static void alarm_scroll_tick(int x, int y, bool valid, uint32_t now_ms,
                               uint32_t elapsed_ms) {
  bool blocked = abs(x) > MOTION_CENTER_MG || app_button_click_pending() ||
                 atomic_load(&s_wait_release) || s_task_pending_id[0];
  if (s_alarm_field != s_selection || !valid || blocked) {
    s_alarm_field = s_selection;
    s_alarm_fraction = 0;
    s_alarm_scroll_armed = false;
    s_alarm_center_since = 0;
    s_alarm_vertical = (motion_gesture_t){.require_stable_center = true};
    if (!valid || blocked) return;
  }
  if (!s_alarm_editing) {
    int direction = motion_gesture_step(&s_alarm_vertical, y, now_ms, valid, false);
    if (direction) {
      // Follow the form's visual rows: hour/minute, repeat, then save.
      s_selection = direction > 0 ? (s_selection < 2 ? 2 : s_selection == 2 ? 3 : 0)
                                  : (s_selection == 3 ? 2 : s_selection == 2 ? 0 : 3);
      app_ui_note_activity();
      s_render_dirty = true;
    }
    return;
  }
  if (s_selection >= 2) {
    // Repeat changes only after explicitly entering its editor.
    int direction = motion_gesture_step(&s_alarm_vertical, y, now_ms, valid, false);
    if (direction && s_alarm_editing && s_selection == 2) {
      s_alarm_repeat = (s_alarm_repeat + 3 + direction) % 3;
      app_ui_note_activity();
      s_render_dirty = true;
    }
    return;
  }
  if (!s_alarm_scroll_armed) {
    if (abs(y) > ALARM_DEADZONE_MG) s_alarm_center_since = 0;
    else if (!s_alarm_center_since) s_alarm_center_since = now_ms;
    else if (now_ms - s_alarm_center_since >= MOTION_CENTER_MS)
      s_alarm_scroll_armed = true;
    return;
  }
  int speed = alarm_speed(y, s_selection ? ALARM_MINUTE_MAX_SPEED : ALARM_HOUR_MAX_SPEED);
  if (!speed || !elapsed_ms) { s_alarm_fraction = 0; return; }
  if ((s_alarm_fraction < 0 && speed > 0) ||
      (s_alarm_fraction > 0 && speed < 0)) s_alarm_fraction = 0;
  if (elapsed_ms > 100) elapsed_ms = 100;
  s_alarm_fraction += speed * (int32_t)elapsed_ms;
  int steps = s_alarm_fraction / 1000;
  s_alarm_fraction %= 1000;
  if (!steps) return;
  // Logical down (+y) increases; the shared sensor layer maps physical axes.
  if (s_selection == 0) s_alarm_hour = (s_alarm_hour + 24 + steps) % 24;
  else s_alarm_minute = (s_alarm_minute + 60 + steps) % 60;
  app_ui_note_activity();
  s_render_dirty = true;
}
static void tick(lv_timer_t *timer) {
  (void)timer;
  int64_t now = esp_timer_get_time();
  app_focus_t *f = &s_preferences.focus;
  if (app_focus_tick(f, now)) {
    s_focus_notice = true;
    snprintf(s_notice_title, sizeof(s_notice_title), "%s",
             f->pomodoro
                 ? (f->resting ? "专注结束，请休息" : "休息结束，可以继续专注")
                 : "倒计时结束");
    save_later();
  }
  if (f->running && now - s_last_save >= 60000000) {
    s_last_save = now;
    save_later();
  }
  if (!s_visible) {
    s_last_frame = now;
    return;
  }
  if (s_menu == MENU_ARCADE) {
    if (app_games_tick()) { enter(MENU_ROOT); render_menu(); }
    return;
  }
  if (s_menu == MENU_ALARM && !s_button_navigation) {
    int x = 0, y = 0;
    bool valid = app_motion_sample(&x, &y);
    uint32_t elapsed_ms = s_last_frame ? (uint32_t)((now - s_last_frame) / 1000) : 0;
    alarm_scroll_tick(x, y, valid, (uint32_t)(now / 1000), elapsed_ms);
  }
  if (grid_menu() && !s_button_navigation && app_motion_available()) {
    int x = 0, y = 0;
    bool valid = app_motion_sample(&x, &y);
    int dir = motion_gesture_step(&s_grid_vertical, y, (uint32_t)(now / 1000),
                                  valid, abs(x) > MOTION_CENTER_MG ||
                                  app_button_click_pending() || atomic_load(&s_wait_release));
    if (dir) {
      s_selection = app_menu_grid_move(s_selection, menu_count(), dir * 2);
      app_ui_note_activity();
      s_render_dirty = true;
    }
  }
  s_last_frame = now;
  if (now - s_last_menu_frame >= 100000) {
    s_last_menu_frame = now;
    render_menu();
  }
}
static bool focus_valid(const app_focus_t *focus) {
  return focus->work_minutes >= 1 && focus->work_minutes <= 120 &&
         focus->break_minutes >= 1 && focus->break_minutes <= 120 &&
         focus->remaining <= 7200;
}
void app_desktop_init(void) {
  // No erase, SD rewrite, or API request. Unknown preference blobs stay intact.
  nvs_handle_t h;
  if (nvs_flash_init() == ESP_OK &&
      nvs_open("desktop", NVS_READONLY, &h) == ESP_OK) {
    size_t size = 0;
    esp_err_t result = nvs_get_blob(h, "preferences", NULL, &size);
    if (result == ESP_OK && size == sizeof(legacy_preferences_t)) {
      legacy_preferences_t old;
      if (nvs_get_blob(h, "preferences", &old, &size) == ESP_OK &&
          (old.version == 1 || old.version == 2) && old.volume <= 100 &&
          focus_valid(&old.focus)) {
        s_preferences.volume = old.volume;
        s_preferences.wake_enabled = old.wake_enabled;
        s_preferences.focus = old.focus;
        memset(s_preferences.reserved_legacy, 0, sizeof(s_preferences.reserved_legacy));
        save_later();
      } else
        s_preferences_writable = false;
    } else if (result == ESP_OK && size == sizeof(preferences_t)) {
      preferences_t loaded;
      if (nvs_get_blob(h, "preferences", &loaded, &size) == ESP_OK &&
          (loaded.version == 3 || loaded.version == 4 || loaded.version == 5) &&
          loaded.volume <= 100 && loaded.brightness <= 100 &&
          focus_valid(&loaded.focus) &&
          (loaded.version < 5 || app_tts_options_valid((app_tts_options_t){
              loaded.tts_rate_percent, loaded.tts_voice_id}))) {
        if (loaded.version < 5) {
          memset(loaded.reserved_legacy, 0, sizeof(loaded.reserved_legacy));
          loaded.tts_rate_percent = 100;
          loaded.tts_voice_id = 0;
          loaded.version = 5;
          save_later();
        }
        s_preferences = loaded;
      } else
        s_preferences_writable = false;
    } else if (result != ESP_ERR_NVS_NOT_FOUND)
      s_preferences_writable = false;
    uint32_t home_style;
    if (nvs_get_u32(h, "home_style", &home_style) == ESP_OK && home_style <= 1)
      s_home_style = home_style;
    uint32_t mode;
    if (nvs_get_u32(h, "wall_mode", &mode) == ESP_OK && mode <= 4)
      s_wallpaper_mode = mode;
    uint32_t learned = 0, cursor = 0;
    if (nvs_get_u32(h, "words_mask", &learned) == ESP_OK)
      s_words.mastered = learned & ((1u << APP_WORD_COUNT) - 1);
    if (nvs_get_u32(h, "words_cursor", &cursor) == ESP_OK && cursor < APP_WORD_COUNT)
      s_words.cursor = cursor;
    nvs_close(h);
  }
  if (s_preferences.focus.running) {
    s_preferences.focus.running = false;
    s_preferences.focus.interrupted = true;
  }
  app_audio_set_volume(s_preferences.volume);
  app_wake_set_enabled(s_preferences.wake_enabled);
  s_commands = xQueueCreate(4, sizeof(task_command_t));
}
void app_desktop_create(lv_obj_t *screen) {
  s_desktop_parent = screen;
  holo_surface(screen);
  s_focus_ring = lv_arc_create(screen);
  lv_obj_set_pos(s_focus_ring, 42, 36);
  lv_obj_set_size(s_focus_ring, 156, 156);
  lv_obj_remove_style(s_focus_ring, NULL, LV_PART_KNOB);
  lv_obj_remove_flag(s_focus_ring, LV_OBJ_FLAG_CLICKABLE);
  lv_arc_set_rotation(s_focus_ring, 270);
  lv_arc_set_bg_angles(s_focus_ring, 0, 360);
  lv_arc_set_range(s_focus_ring, 0, 100);
  lv_obj_set_style_arc_width(s_focus_ring, 4, LV_PART_MAIN);
  lv_obj_set_style_arc_color(s_focus_ring, lv_color_hex(0x383828), LV_PART_MAIN);
  lv_obj_set_style_arc_width(s_focus_ring, 6, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(s_focus_ring, lv_color_hex(HOLO_MINT), LV_PART_INDICATOR);
  if (app_display_set_brightness(s_preferences.brightness) != ESP_OK)
    feedback("亮度设置未应用");
  s_title = holo_label(screen, "", &app_ui_font_20, HOLO_CREAM, 20, 20, 200,
                       LV_TEXT_ALIGN_CENTER);
  s_body = holo_label(screen, "", &app_ui_font_16, HOLO_CREAM, 20, 134, 200,
                      LV_TEXT_ALIGN_CENTER);
  lv_label_set_long_mode(s_body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_style_text_line_space(s_body, 5, 0);
  s_hint = holo_label(screen, "", &app_ui_font_14, HOLO_APRICOT, 5, 217, 230,
                      LV_TEXT_ALIGN_CENTER);
  s_value = holo_label(screen, "", &lv_font_montserrat_48, HOLO_CREAM, 20, 65,
                       200, LV_TEXT_ALIGN_CENTER);
  s_position = holo_label(screen, "", &app_ui_font_16, HOLO_APRICOT, 20, 177,
                          200, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_y(s_position, 218);
  s_grid = holo_rect(screen, 8, 43, 224, 168, HOLO_INK, 0, false, 0);
  for (unsigned i = 0; i < 4; i++) {
    s_grid_tiles[i] = holo_rect(s_grid, (int)(i % 3) * 75, (int)(i / 3) * 56,
                                71, 51, HOLO_APRICOT, 6, false, 1);
    s_grid_names[i] = holo_label(s_grid_tiles[i], "", &app_ui_font_14,
                                 HOLO_CREAM, 3, 10, 65, LV_TEXT_ALIGN_CENTER);
    lv_label_set_long_mode(s_grid_names[i], LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_height(s_grid_names[i], 34);
    s_grid_icons[i] = holo_label(s_grid_tiles[i], "", &lv_font_montserrat_32,
                                HOLO_MINT, 6, 16, 96, LV_TEXT_ALIGN_CENTER);
  }
  s_qr = lv_qrcode_create(screen);
  lv_qrcode_set_size(s_qr, 142);
  lv_qrcode_set_dark_color(s_qr, lv_color_black());
  lv_qrcode_set_light_color(s_qr, lv_color_white());
  lv_qrcode_set_quiet_zone(s_qr, true);
  lv_obj_set_pos(s_qr, 49, 43);
  show(s_qr, false);
  for (unsigned i = 0; i < 3; i++) {
    s_rows[i] = holo_label(screen, "", &app_ui_font_20, HOLO_CREAM, 20,
                           96 + i * 33, 200, LV_TEXT_ALIGN_CENTER);
    s_tool_icons[i] = holo_tool_icon(screen, i, 80, 56);
  }
  s_robot = holo_robot_create(screen, 96, 75, 1);
  lv_timer_create(tick, 33, NULL);
  render_menu();
}
void app_desktop_open_root(void) {
  enter(MENU_ROOT);
  if ((unsigned)s_selection >= menu_count())
    s_selection = (int)menu_count() - 1;
}
void app_desktop_open_tool(unsigned index) {
  if (index >= sizeof(tool_items) / sizeof(*tool_items)) return;
  s_selections[MENU_ROOT] = (int)index;
  if (index >= 4) {
    enter(MENU_ARCADE);
    app_games_open(s_desktop_parent, index - 4);
    return;
  }
  enter(index == 0 ? MENU_FOCUS : index == 1 ? MENU_WORDS
        : index == 2 ? MENU_TASKS : MENU_MEDIA);
  if (index == 0) { s_selection = 0; s_focus_confirm = false; }
  if (index == 1 && !s_words.count) { app_words_begin(&s_words); s_words_dirty = true; }
}
void app_desktop_open_settings(void) {
  enter(MENU_SETTINGS);
}
void app_desktop_open_clock(void) {
  time_t now = time(NULL);
  struct tm local;
  bool time_valid = now >= 1704067200 && localtime_r(&now, &local);
  s_alarm_hour = time_valid ? (unsigned)local.tm_hour : 7;
  s_alarm_minute = time_valid ? ((unsigned)local.tm_min + 1) % 60 : 0;
  if (time_valid && !s_alarm_minute) s_alarm_hour = (s_alarm_hour + 1) % 24;
  s_alarm_repeat = 0;
  s_alarm_editing = false;
  s_alarm_field = -1;
  enter(MENU_ALARM);
  s_selection = 0;
  if (!time_valid) feedback("请先联网校时");
}
void app_desktop_visible(bool visible) {
  if (visible && !s_visible)
    s_button_navigation = !app_motion_available();
  if (!visible && s_visible) {
    if (s_menu == MENU_ARCADE) enter(MENU_ROOT);
    // Leaving an unconfirmed editor (including voice wake) discards preview.
    if (s_menu == MENU_VOLUME)
      app_audio_set_volume(s_preferences.volume);
    else if (s_menu == MENU_BRIGHTNESS &&
             app_display_set_brightness(s_preferences.brightness) != ESP_OK)
      feedback("亮度恢复失败");
  }
  s_visible = visible;
  s_render_dirty = true;
}
static void toggle_task(void) {
  if (!s_tasks_ready || !s_count) {
    feedback("暂无任务");
    return;
  }
  if (s_task_pending_id[0]) {
    feedback("任务正在保存");
    return;
  }
  unsigned index = s_menu == MENU_TASK ? (unsigned)s_selections[MENU_TASKS]
                                       : (unsigned)s_selection;
  if (index >= s_count)
    return;
  const app_task_view_t *task = &s_tasks[index];
  bool reopen = !strcmp(task->status, "completed");
  if (reopen && !strcmp(task->kind, "alarm") &&
      task->due_utc <= (long long)time(NULL)) {
    feedback("过期提醒不能重新启用");
    return;
  }
  task_command_t command = {0};
  snprintf(command.id, sizeof(command.id), "%s", task->id);
  snprintf(command.action, sizeof(command.action), "%s",
           reopen ? "reopen" : "complete");
  if (s_commands && xQueueSend(s_commands, &command, 0) == pdTRUE) {
    snprintf(s_task_pending_id, sizeof(s_task_pending_id), "%s", task->id);
    feedback("正在保存任务");
  } else
    feedback("操作忙，请重试");
}
static void activate(void) {
  unsigned count = menu_count();
  if (s_selection < 0 || (unsigned)s_selection >= count)
    s_selection = 0;
  switch (s_menu) {
  case MENU_ALARM:
    if (s_task_pending_id[0]) {
      feedback("闹钟正在保存");
      break;
    }
    if (s_selection < 3) {
      if (!s_alarm_editing) {
        s_edit_value = s_selection == 0 ? s_alarm_hour :
                       s_selection == 1 ? s_alarm_minute : s_alarm_repeat;
        s_alarm_editing = true;
      } else
        s_alarm_editing = false;
      s_alarm_field = -1;
      break;
    }
    if (time(NULL) < 1704067200) {
      feedback("请先联网校时");
      break;
    }
    if (!s_tasks_ready || s_count >= 8) {
      feedback(s_count >= 8 ? "任务已满，请先删除旧任务"
                            : "任务存储不可用，请检查SD卡");
      break;
    }
    task_command_t alarm = {.hour = s_alarm_hour,
                            .minute = s_alarm_minute,
                            .repeat = s_alarm_repeat};
    snprintf(alarm.action, sizeof(alarm.action), "create_alarm");
    if (s_commands && xQueueSend(s_commands, &alarm, 0) == pdTRUE) {
      snprintf(s_task_pending_id, sizeof(s_task_pending_id), "clock");
      feedback("正在保存闹钟");
    } else
      feedback("操作忙，请重试");
    break;
  case MENU_ROOT:
    app_desktop_open_tool((unsigned)s_selection);
    break;
  case MENU_TASKS:
    if (s_count) {
      enter(MENU_TASK);
      s_selection = 0;
    }
    break;
  case MENU_TASK:
    toggle_task();
    break;
  case MENU_FOCUS: {
    app_focus_t *f = &s_preferences.focus;
    if (s_focus_confirm) {
      if (s_selection == 1) {
        f->remaining = 0;
        f->running = f->resting = f->interrupted = false;
        s_focus_notice = false;
      }
      s_focus_confirm = false;
      s_selection = 0;
    } else if (s_selection == 1) {
      if (f->remaining || f->running) s_focus_confirm = true;
      else enter(MENU_ROOT);
      s_selection = 0;
    } else if (!f->remaining && !f->running)
      app_focus_start(f, f->work_minutes, true, esp_timer_get_time());
    else
      app_focus_toggle(f, esp_timer_get_time());
    save_later();
    break;
  }
  case MENU_WORDS:
    if (!app_words_current(&s_words)) {
      app_words_begin(&s_words);
      s_words_dirty = true;
    } else if (!s_words.revealed) s_words.revealed = true;
    else if (app_words_grade(&s_words, s_selection == 0)) s_words_dirty = true;
    s_selection = 0;
    break;
  case MENU_SETTINGS:
    if (s_selection == 0 || s_selection == 1) {
      s_edit_value =
          s_selection == 0 ? s_preferences.brightness : s_preferences.volume;
      enter(s_selection == 0 ? MENU_BRIGHTNESS : MENU_VOLUME);
    } else if (s_selection == 2)
      enter(MENU_APPEARANCE);
    else if (s_selection == 6) {
      enter(MENU_CALIBRATION);
      s_calibration_finished = false;
      s_calibration_waiting = app_motion_request_calibration();
      feedback(s_calibration_waiting ? "放平静置一秒" : "传感器不可用");
    } else if (s_selection == 3) {
      s_edit_value = s_preferences.focus.work_minutes;
      enter(MENU_WORK_TIME);
    } else if (s_selection == 4) {
      s_edit_value = s_preferences.focus.break_minutes;
      enter(MENU_BREAK_TIME);
    } else if (s_selection == 5)
      enter(MENU_DIRECTION);
    else if (s_selection == 7)
      enter(MENU_STORAGE);
    break;
  case MENU_VOLUME:
    s_preferences.volume = s_edit_value;
    save_later();
    enter(MENU_SETTINGS);
    feedback("音量已应用");
    break;
  case MENU_BRIGHTNESS:
    s_preferences.brightness = s_edit_value;
    save_later();
    enter(MENU_SETTINGS);
    feedback("亮度已应用");
    break;
  case MENU_WORK_TIME:
  case MENU_BREAK_TIME:
    if (s_menu == MENU_WORK_TIME)
      s_preferences.focus.work_minutes = s_edit_value;
    else
      s_preferences.focus.break_minutes = s_edit_value;
    save_later();
    enter(MENU_SETTINGS);
    feedback("时长已应用");
    break;
  case MENU_CALIBRATION:
    s_calibration_finished = false;
    s_calibration_waiting = app_motion_request_calibration();
    feedback(s_calibration_waiting ? "放平静置一秒" : "传感器不可用");
    break;
  case MENU_MEDIA:
    if (s_selection == 0)
      app_ui_open_album();
    else if (s_selection == 1 || s_selection == 2) {
      s_ambient_browser = s_selection == 2;
      enter(MENU_MUSIC);
    } else if (s_selection == 3) {
      open_wallpapers(true);
    } else
      enter(MENU_WEB);
    break;
  case MENU_MUSIC: {
    app_media_snapshot(&s_media);
    app_media_command_t command;
    unsigned argument = 0;
    if (s_selection == 0) {
      enter(MENU_TRACKS);
      s_selection = (int)audio_selection();
      break;
    }
    if (s_selection == 6) {
      enter(MENU_MUSIC_TIMER);
      break;
    }
    if (!audio_count(s_ambient_browser)) {
      feedback(s_ambient_browser ? "请先安装环境音" : "请先安装音乐");
      break;
    }
    if (s_selection == 1) {
      command = !audio_current()  ? APP_MEDIA_PLAY
                : s_media.playing ? APP_MEDIA_PAUSE
                : s_media.paused  ? APP_MEDIA_RESUME
                                  : APP_MEDIA_PLAY;
      argument = audio_current() ? s_media.audio_index : audio_index(0);
    } else if (s_selection == 2 || s_selection == 3) {
      command = !audio_current()   ? APP_MEDIA_PLAY
                : s_selection == 2 ? APP_MEDIA_PREVIOUS
                                   : APP_MEDIA_NEXT;
      argument = audio_index(0);
    } else if (s_selection == 4)
      command = APP_MEDIA_STOP;
    else {
      if (!audio_current()) {
        feedback("请先播放所选分类");
        break;
      }
      command = APP_MEDIA_REPEAT;
      argument = !s_media.repeat;
    }
    feedback(app_media_command(command, argument) ? "正在应用"
                                                  : "媒体忙，请重试");
    break;
  }
  case MENU_TRACKS:
    if (!audio_count(s_ambient_browser))
      feedback(s_ambient_browser ? "请先安装环境音" : "请先上传WAV音乐");
    else if (app_media_command(APP_MEDIA_PLAY,
                               audio_index((unsigned)s_selection))) {
      enter(MENU_MUSIC);
      s_selection = 1;
      feedback("正在播放所选曲目");
    } else
      feedback("媒体忙，请重试");
    break;
  case MENU_MUSIC_TIMER:
    if (app_media_command(APP_MEDIA_AMBIENT_TIMER,
                          music_minutes[s_selection])) {
      enter(MENU_MUSIC);
      s_selection = 6;
      feedback("定时设置已提交");
    } else
      feedback("媒体忙，请重试");
    break;
  case MENU_WEB:
    app_media_web_status(&s_web);
    app_media_web_request(!s_web.enabled);
    feedback(s_web.enabled ? "正在关闭网页管理" : "正在开启网页管理");
    break;
  case MENU_APPEARANCE:
    app_media_get_appearance(&s_appearance);
    if (s_selection < 2) {
      select_home_style((unsigned)s_selection);
      feedback(s_home_style && !s_wallpaper_mode && !s_media.image_count
                   ? "暂无壁纸，先显示时间" : "主页样式已应用");
      break;
    }
    if (s_selection == 2) {
      open_wallpapers(false);
      break;
    }
    if (s_selection == 3) {
      s_appearance.night_enabled = !s_appearance.night_enabled;
      s_appearance.night_start_minute = 22 * 60;
      s_appearance.night_end_minute = 7 * 60;
      s_appearance.night_brightness = 20;
    } else if (s_selection == 4)
      s_appearance.idle_seconds = s_appearance.idle_seconds ? 0 : 60;
    feedback(app_media_set_appearance(&s_appearance) ? "外观设置已提交"
                                                     : "媒体忙，请重试");
    break;
  case MENU_WALLPAPER:
    if (s_selection < WALLPAPER_DYNAMIC_FIRST) {
      select_home_style((unsigned)s_selection);
      feedback(s_home_style && !s_wallpaper_mode && !s_media.image_count
                   ? "暂无壁纸，先显示时间" : "主页样式已应用");
      break;
    }
    if (s_selection < WALLPAPER_IMAGE_FIRST) {
      app_desktop_select_wallpaper_mode(
          (unsigned)s_selection - WALLPAPER_DYNAMIC_FIRST + 1);
      feedback("动态壁纸已选定");
      break;
    }
    if (s_media.image_count && app_media_select_image(
                                  (unsigned)s_selection - WALLPAPER_IMAGE_FIRST,
                                  true)) {
      app_desktop_select_wallpaper_mode(0);
      feedback("壁纸已选定");
    } else
      feedback("暂无可用图片");
    break;
  default:
    break;
  }
}
int app_desktop_button(bool long_press) {
  if (!s_visible)
    return 0;
  s_render_dirty = true;
  s_feedback[0] = 0;
  if (s_button_navigation) {
    if (long_press) {
      atomic_store(&s_wait_release, true);
      activate();
    } else
      menu_move(1);
  } else if (!long_press) {
    atomic_store(&s_wait_release, false);
    activate();
  }
  // Normal menus consume holds without saving. Only the explicit, session-
  // latched sensorless mode uses a hold to confirm.
  return 1;
}
unsigned app_desktop_base_brightness(void) {
  return s_visible && s_menu == MENU_BRIGHTNESS ? s_edit_value
                                                : s_preferences.brightness;
}
int app_desktop_back(void) {
  if (!s_visible)
    return 0;
  s_feedback[0] = 0;
  s_render_dirty = true;
  switch (s_menu) {
  case MENU_ALARM:
    if (s_alarm_editing) {
      if (s_selection == 0) s_alarm_hour = s_edit_value;
      else if (s_selection == 1) s_alarm_minute = s_edit_value;
      else s_alarm_repeat = s_edit_value;
      s_alarm_editing = false;
      s_alarm_field = -1;
      return 1;
    }
    return 3;
  case MENU_ROOT:
    return 3;
  case MENU_ARCADE:
    enter(MENU_ROOT);
    break;
  case MENU_SETTINGS:
    return 3;
  case MENU_FOCUS:
    if (s_focus_confirm) { s_focus_confirm = false; s_selection = 0; break; }
    enter(MENU_ROOT);
    break;
  case MENU_WORDS:
  case MENU_TASKS:
  case MENU_MEDIA:
  case MENU_ANNIVERSARIES:
    enter(MENU_ROOT);
    break;
  case MENU_VOLUME:
    app_audio_set_volume(s_preferences.volume);
    enter(MENU_SETTINGS);
    feedback("音量修改已撤销");
    break;
  case MENU_BRIGHTNESS:
    if (app_display_set_brightness(s_preferences.brightness) != ESP_OK) {
      enter(MENU_SETTINGS);
      feedback("亮度恢复失败");
      break;
    }
    enter(MENU_SETTINGS);
    feedback("亮度修改已撤销");
    break;
  case MENU_DIRECTION:
  case MENU_CALIBRATION:
  case MENU_STORAGE:
  case MENU_APPEARANCE:
  case MENU_WORK_TIME:
  case MENU_BREAK_TIME:
    enter(MENU_SETTINGS);
    break;
  case MENU_MUSIC:
  case MENU_WEB:
    enter(MENU_MEDIA);
    break;
  case MENU_TRACKS:
  case MENU_MUSIC_TIMER:
    enter(MENU_MUSIC);
    break;
  case MENU_WALLPAPER:
    enter(s_wallpaper_from_media ? MENU_MEDIA : MENU_APPEARANCE);
    break;
  case MENU_TASK:
  case MENU_TASK_ACTION:
  case MENU_CONFIRM:
    enter(MENU_TASKS);
    break;
  default:
    return 3;
  }
  return 1;
}
static bool menu_move(int direction) {
  if (!s_visible)
    return false;
  if (s_menu == MENU_ALARM && s_task_pending_id[0])
    return true;
  if (s_menu == MENU_ALARM && s_alarm_editing) {
    // Horizontal gestures cannot steal focus from a value being edited.
    // Emergency button-only navigation uses the same draft and confirmation.
    if (s_button_navigation) {
      if (s_selection == 0) s_alarm_hour = (s_alarm_hour + 1) % 24;
      else if (s_selection == 1) s_alarm_minute = (s_alarm_minute + 1) % 60;
      else s_alarm_repeat = (s_alarm_repeat + 1) % 3;
      s_render_dirty = true;
    }
    return true;
  }
  if (s_menu == MENU_VOLUME || s_menu == MENU_BRIGHTNESS ||
      s_menu == MENU_WORK_TIME || s_menu == MENU_BREAK_TIME) {
    bool percent = s_menu == MENU_VOLUME || s_menu == MENU_BRIGHTNESS;
    int minimum = percent ? 0 : 1;
    int maximum = percent ? 100 : 120;
    int value = (int)s_edit_value + (direction < 0 ? -1 : 1) * (percent ? 5 : 1);
    if (s_button_navigation && value > maximum)
      value = minimum;
    s_edit_value = (unsigned)(value < minimum ? minimum :
                              value > maximum ? maximum : value);
    if (s_menu == MENU_VOLUME)
      app_audio_set_volume(s_edit_value);
    else if (s_menu == MENU_BRIGHTNESS &&
             app_display_set_brightness(s_edit_value) != ESP_OK) {
      s_edit_value = app_display_get_brightness();
      feedback("亮度调整失败");
    }
    s_render_dirty = true;
  } else {
    unsigned count = menu_count();
    if (count)
      s_selection = grid_menu() ? app_menu_grid_move(s_selection, count, direction < 0 ? -1 : 1) :
          (s_selection + (direction < 0 ? -1 : 1) + (int)count) % (int)count;
    s_render_dirty = true;
  }
  return true;
}
bool app_desktop_motion(int direction) {
  if (!s_visible)
    return false;
  // Only explicit button navigation moves a fault-mode menu, even if sensor
  // frames recover. Leave/re-enter the menu to select the normal mode again.
  return s_button_navigation ? true : menu_move(direction);
}
static esp_err_t persist_settings(const preferences_t *snapshot,
                                  unsigned home_style,
                                  unsigned wallpaper_mode) {
  nvs_handle_t h;
  esp_err_t result = nvs_open("desktop", NVS_READWRITE, &h);
  if (result == ESP_OK) {
    result = nvs_set_blob(h, "preferences", snapshot, sizeof(*snapshot));
    if (result == ESP_OK)
      result = nvs_set_u32(h, "home_style", home_style);
    if (result == ESP_OK)
      result = nvs_set_u32(h, "wall_mode", wallpaper_mode);
    if (result == ESP_OK)
      result = nvs_commit(h);
    nvs_close(h);
  }
  return result;
}

void app_desktop_service(void) {
  task_command_t command;
  if (!s_task_result_waiting && s_commands &&
      xQueueReceive(s_commands, &command, 0) == pdTRUE) {
    bool create_alarm = !strcmp(command.action, "create_alarm");
    bool ok =
        create_alarm
            ? command.repeat < 3 &&
                  app_agent_create_alarm(command.hour, command.minute,
                                         alarm_repeats[command.repeat])
            : app_agent_task_change(command.id, command.action, 0, NULL, NULL);
    snprintf(s_task_outcome, sizeof(s_task_outcome), "%s",
             ok ? (create_alarm ? "闹钟已保存" : "任务已保存")
                : "保存失败，请检查SD、校时和任务上限");
    s_task_result_waiting = true;
  }
  app_task_view_t rows[8];
  bool rows_ready = app_cloud_local_ready();
  unsigned count = app_agent_task_snapshot(rows);
  unsigned bytes = 0, archives = 0;
  bool capacity_check =
      esp_timer_get_time() - s_capacity_at > 10000000 || !s_capacity_at;
  bool storage_ready =
      capacity_check && app_storage_capacity(&bytes, &archives);
  if (capacity_check)
    s_capacity_at = esp_timer_get_time();
  preferences_t snapshot;
  unsigned home_style = 0, wallpaper_mode = 0;
  bool save = false;
  bool save_words = false;
  // 仅由service任务访问；即使UI锁繁忙，也不会丢失Flash失败重试。
  static bool words_save_failed;
  uint32_t words_mask = 0, words_cursor = 0;
  if (lvgl_port_lock(20)) {
    if (words_save_failed) {
      s_words_dirty = true;
      s_words_retry_at = esp_timer_get_time() + 30000000;
      feedback("词卡进度暂未保存");
      words_save_failed = false;
    }
    char selected[64] = {0};
    if (s_menu == MENU_TASKS && (unsigned)s_selection < s_count)
      snprintf(selected, sizeof(selected), "%s", s_tasks[s_selection].id);
    if (rows_ready != s_tasks_ready || count != s_count ||
        memcmp(s_tasks, rows, count * sizeof(*rows)))
      s_render_dirty = true;
    memcpy(s_tasks, rows, count * sizeof(*rows));
    s_count = count;
    s_tasks_ready = rows_ready;
    if (selected[0])
      for (unsigned i = 0; i < count; i++)
        if (!strcmp(rows[i].id, selected)) {
          s_selection = (int)i;
          s_selections[MENU_TASKS] = (int)i;
          break;
        }
    if (capacity_check) {
      s_storage_ready = storage_ready;
      s_journal_bytes = bytes;
      s_archives = archives;
      s_render_dirty = true;
    }
    if (s_task_result_waiting) {
      s_task_pending_id[0] = 0;
      feedback(s_task_outcome);
      s_task_result_waiting = false;
    }
    bool dirty = s_dirty;
    if (s_words_dirty && esp_timer_get_time() >= s_words_retry_at) {
      save_words = true;
      words_mask = s_words.mastered; words_cursor = s_words.cursor;
      s_words_dirty = false;
    }
    save = dirty && s_preferences_writable;
    s_dirty = false;
    snapshot = s_preferences;
    home_style = s_home_style;
    wallpaper_mode = s_wallpaper_mode;
    snapshot.focus.remaining =
        app_focus_remaining(&s_preferences.focus, esp_timer_get_time());
    if (dirty && !s_preferences_writable)
      feedback("保留旧设置，暂时生效");
    lvgl_port_unlock();
  }
  if (save) {
    esp_err_t result = persist_settings(&snapshot, home_style, wallpaper_mode);
    if (result != ESP_OK && lvgl_port_lock(20)) {
      feedback("保存失败，暂时生效");
      lvgl_port_unlock();
    }
  }
  // 独立键避免改变历史偏好blob布局；所有Flash写入均在LVGL锁外。
  if (save_words) {
    nvs_handle_t h;
    esp_err_t result = nvs_open("desktop", NVS_READWRITE, &h);
    if (result == ESP_OK) {
      result = nvs_set_u32(h, "words_mask", words_mask);
      if (result == ESP_OK) result = nvs_set_u32(h, "words_cursor", words_cursor);
      if (result == ESP_OK) result = nvs_commit(h);
      nvs_close(h);
    }
    words_save_failed = result != ESP_OK;
  }
}
unsigned app_desktop_home_style(void) { return s_home_style; }
unsigned app_desktop_wallpaper_mode(void) { return s_wallpaper_mode; }
void app_desktop_select_wallpaper_mode(unsigned mode) {
  if (mode <= 4) {
    s_wallpaper_mode = mode;
    select_home_style(1);
  }
}
// 统一配置快照入口。UI中的直接编辑也会被下次读取观察到；逐字段比较
// 不依赖结构体padding。启动随机版本令旧网页在设备重启后需要重新读取。
static app_desktop_config_t configuration_locked(void) {
  static app_desktop_config_t previous;
  static uint32_t revision;
  app_desktop_config_t current = {
      .home_style = s_home_style, .wallpaper_mode = s_wallpaper_mode,
      .volume = s_preferences.volume, .brightness = s_preferences.brightness,
      .wake_enabled = s_preferences.wake_enabled,
      .work_minutes = s_preferences.focus.work_minutes,
      .break_minutes = s_preferences.focus.break_minutes,
      .tts_rate_percent = s_preferences.tts_rate_percent,
      .tts_voice_id = s_preferences.tts_voice_id};
  bool changed = current.home_style != previous.home_style ||
      current.wallpaper_mode != previous.wallpaper_mode ||
      current.volume != previous.volume || current.brightness != previous.brightness ||
      current.wake_enabled != previous.wake_enabled ||
      current.work_minutes != previous.work_minutes ||
      current.break_minutes != previous.break_minutes ||
      current.tts_rate_percent != previous.tts_rate_percent ||
      current.tts_voice_id != previous.tts_voice_id;
  if (!revision) revision = esp_random() | 1u;
  else if (changed && ++revision == 0) revision = 1;
  current.revision = revision;
  previous = current;
  return current;
}

static bool configuration_valid(const app_desktop_config_t *set) {
  return set->home_style <= 1 && set->wallpaper_mode <= 4 &&
      set->volume <= 100 && set->brightness <= 100 &&
      app_tts_options_valid((app_tts_options_t){set->tts_rate_percent, set->tts_voice_id}) &&
      set->work_minutes >= 1 && set->work_minutes <= 120 &&
      set->break_minutes >= 1 && set->break_minutes <= 120;
}

app_config_result_t app_desktop_configuration_patch(
    app_desktop_config_t *out, const app_desktop_config_t *patch,
    unsigned mask, uint32_t expected_revision) {
  if (!patch || !mask || (mask & ~APP_CONFIG_ALL)) return APP_CONFIG_INVALID;
  if (!lvgl_port_lock(100)) return APP_CONFIG_BUSY;
  app_desktop_config_t merged = configuration_locked();
  if (expected_revision && merged.revision != expected_revision) {
    if (out) *out = merged;
    lvgl_port_unlock();
    return APP_CONFIG_CONFLICT;
  }
  // 合并、校验、应用在同一把UI锁内完成。不能在锁外用旧整份配置覆盖。
#define MERGE(field, flag) if (mask & (flag)) merged.field = patch->field
  MERGE(home_style, APP_CONFIG_HOME);
  MERGE(wallpaper_mode, APP_CONFIG_WALLPAPER);
  MERGE(volume, APP_CONFIG_VOLUME);
  MERGE(brightness, APP_CONFIG_BRIGHTNESS);
  MERGE(wake_enabled, APP_CONFIG_WAKE);
  MERGE(work_minutes, APP_CONFIG_WORK);
  MERGE(break_minutes, APP_CONFIG_BREAK);
  MERGE(tts_rate_percent, APP_CONFIG_TTS_RATE);
  MERGE(tts_voice_id, APP_CONFIG_TTS_VOICE);
#undef MERGE
  if (!s_preferences_writable || !configuration_valid(&merged)) {
    lvgl_port_unlock();
    return APP_CONFIG_INVALID;
  }
  // 先执行可能失败的硬件操作，失败时不先改其他运行值。
  if ((mask & APP_CONFIG_BRIGHTNESS) &&
      app_display_set_brightness(merged.brightness) != ESP_OK) {
    lvgl_port_unlock();
    return APP_CONFIG_BUSY;
  }
  const app_desktop_config_t *set = &merged;
  preferences_t snapshot = {0};
  bool visual_changed = s_home_style != set->home_style ||
                        s_wallpaper_mode != set->wallpaper_mode;
  s_home_style = set->home_style;
  s_wallpaper_mode = set->wallpaper_mode;
  s_preferences.volume = set->volume;
  s_preferences.tts_rate_percent = set->tts_rate_percent;
  s_preferences.tts_voice_id = set->tts_voice_id;
  s_preferences.brightness = set->brightness;
  s_preferences.wake_enabled = set->wake_enabled;
  s_preferences.focus.work_minutes = set->work_minutes;
  s_preferences.focus.break_minutes = set->break_minutes;
  // 只覆盖这次明确修改的编辑项，不能让改语速顺带丢掉未确认的音量草稿。
  if (s_menu == MENU_VOLUME && (mask & APP_CONFIG_VOLUME)) s_edit_value = set->volume;
  else if (s_menu == MENU_BRIGHTNESS && (mask & APP_CONFIG_BRIGHTNESS)) s_edit_value = set->brightness;
  else if (s_menu == MENU_WORK_TIME && (mask & APP_CONFIG_WORK)) s_edit_value = set->work_minutes;
  else if (s_menu == MENU_BREAK_TIME && (mask & APP_CONFIG_BREAK)) s_edit_value = set->break_minutes;
  if (mask & APP_CONFIG_VOLUME) app_audio_set_volume(set->volume);
  if (mask & APP_CONFIG_WAKE) app_wake_set_enabled(set->wake_enabled);
  snapshot = s_preferences;
  snapshot.focus.remaining =
      app_focus_remaining(&s_preferences.focus, esp_timer_get_time());
  s_dirty = false;
  s_render_dirty = true;
  if (visual_changed) app_ui_preview_home();
  app_desktop_config_t actual = configuration_locked();
  if (out) *out = actual;
  lvgl_port_unlock();
  // NVS有慢I/O，必须在UI锁外；协调任务保证持久化操作与语音写操作互斥。
  bool saved = persist_settings(&snapshot, set->home_style,
                                set->wallpaper_mode) == ESP_OK;
  if (!saved && lvgl_port_lock(100)) {
    feedback("保存失败，暂时生效");
    lvgl_port_unlock();
  }
  return saved ? APP_CONFIG_SAVED : APP_CONFIG_SAVE_FAILED;
}
bool app_desktop_configuration(app_desktop_config_t *out,
                               const app_desktop_config_t *set) {
  if (set)
    return app_desktop_configuration_patch(out, set, APP_CONFIG_ALL, 0) == APP_CONFIG_SAVED;
  if (!lvgl_port_lock(100)) return false;
  if (out) *out = configuration_locked();
  lvgl_port_unlock();
  return true;
}
bool app_desktop_tts_options(app_tts_options_t *out) {
  if (!out || !lvgl_port_lock(100)) return false;
  *out = (app_tts_options_t){s_preferences.tts_rate_percent,
                           s_preferences.tts_voice_id};
  lvgl_port_unlock();
  return app_tts_options_valid(*out);
}
bool app_desktop_status(app_desktop_status_t *out) {
  if (!out || !lvgl_port_lock(100)) return false;
  *out = (app_desktop_status_t){
      .focus_seconds = app_focus_remaining(&s_preferences.focus, esp_timer_get_time()),
      .work_minutes = s_preferences.focus.work_minutes,
      .break_minutes = s_preferences.focus.break_minutes,
      .focus_running = s_preferences.focus.running,
      .focus_resting = s_preferences.focus.resting};
  lvgl_port_unlock();
  return true;
}
bool app_desktop_focus_control(const char *action, unsigned minutes, bool pomodoro) {
  app_focus_t *f = &s_preferences.focus;
  if (!strcmp(action, "focus_start")) {
    if (minutes > 120) return false;
    app_focus_start(f, minutes ? minutes : f->work_minutes, pomodoro, esp_timer_get_time());
    s_focus_notice = false;
  } else if (!strcmp(action, "focus_pause")) {
    if (!app_focus_remaining(f, esp_timer_get_time())) return false;
    if (f->running) app_focus_toggle(f, esp_timer_get_time());
  } else if (!strcmp(action, "focus_resume")) {
    if (!app_focus_remaining(f, esp_timer_get_time())) return false;
    if (!f->running) app_focus_toggle(f, esp_timer_get_time());
  } else if (!strcmp(action, "focus_stop")) {
    f->running = f->resting = f->interrupted = false;
    f->remaining = 0;
    s_focus_notice = false;
  } else return false;
  save_later();
  return true;
}
void app_desktop_open_voice_menu(const char *target) {
  if (!strcmp(target, "settings")) app_desktop_open_settings();
  else if (!strcmp(target, "alarm")) app_desktop_open_clock();
  else if (!strcmp(target, "wallpapers")) open_wallpapers(true);
  else if (!strcmp(target, "web")) enter(MENU_WEB);
  else if (!strcmp(target, "music") || !strcmp(target, "ambient")) {
    s_ambient_browser = !strcmp(target, "ambient");
    enter(MENU_MUSIC);
  } else if (!strcmp(target, "anniversaries")) enter(MENU_ANNIVERSARIES);
  else app_desktop_open_tool(!strcmp(target, "focus") ? 0 :
         !strcmp(target, "tasks") ? 2 : !strcmp(target, "media") ? 3 : 1);
}
bool app_desktop_take_focus_notice(char title[128]) {
  bool ready = false;
  if (lvgl_port_lock(20)) {
    ready = s_focus_notice;
    s_focus_notice = false;
    if (ready)
      snprintf(title, 128, "%s", s_notice_title);
    lvgl_port_unlock();
  }
  return ready;
}
void app_desktop_home_text(char text[192]) {
  app_focus_t *f = &s_preferences.focus;
  if (s_focus_notice) {
    snprintf(text, 192, "%s", s_notice_title);
    return;
  }
  if (f->running) {
    unsigned n = app_focus_remaining(f, esp_timer_get_time());
    snprintf(text, 192, "%s %02u:%02u", f->resting ? "休息" : "专注", n / 60,
             n % 60);
    return;
  }
  if (!s_tasks_ready) {
    snprintf(text, 192, "正在读取提醒");
    return;
  }
  int best = -1;
  for (unsigned i = 0; i < s_count; i++)
    if (!strcmp(s_tasks[i].status, "pending") && s_tasks[i].due_utc &&
        (best < 0 || s_tasks[i].due_utc < s_tasks[best].due_utc))
      best = (int)i;
  if (best >= 0) {
    char due[32];
    format_due(due, s_tasks[best].due_utc);
    snprintf(text, 192, "%s %s",
             time(NULL) >= s_tasks[best].due_utc ? "等待提醒" : due,
             s_tasks[best].title);
  } else
    snprintf(text, 192, "今天暂无提醒");
}
char *app_desktop_voice_command(const char *text) {
  // Exact common commands remain a fast path, sharing tracked execution and
  // confirmation with the structured Agent tools instead of bypassing them.
  return app_control_voice_command(text);
}
