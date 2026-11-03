/*
 * LVGL页面导航/展示：时钟、天气、语音状态与桌面工具层。
 * 业务结果按设备回执核验；对象由UI锁保护，慢I/O由后台处理。
 */
#include "esp_attr.h"
#include "app_ui.h"
#include "app_acoustic.h"
#include "app_button.h"
#include "app_desktop.h"
#include "app_games.h"
#include "app_display.h"
#include "app_log.h"
#include "app_media.h"
#include "app_memory_alloc.h"
#include "app_motion.h"
#include "app_motion_gesture.h"
#include "app_network.h"
#include "app_ui_theme.h"

#include <math.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"

#define MIC_UI_NOISE_FLOOR 12000
#define MIC_UI_FULL_SCALE 300000
#define MIC_UI_REFRESH_MS 100
#define PAGE_COUNT 4
#define NO_VOICE_STATE 6
#define FORECAST_COLUMN_PX 57
#define FORECAST_SLOTS 5 // Four visible hours plus the entering partial column.
#define FORECAST_DEADZONE_MG 120
#define FORECAST_FULL_TILT_MG 600
#define FORECAST_MIN_SPEED 30
#define FORECAST_MAX_SPEED 300

static const char *TAG = "app_ui";
static const char *PAGE_TITLES[] = {"时钟", "天气", "语音助手", "桌面工具"};
static const char *VOICE_STATES[] = {
    "正在聆听", "正在处理", "正在回答", "未能完成", "提醒中", "已取消",
};
static _Atomic uint32_t s_mic_level;
static _Atomic bool s_wifi_connected;
static lv_obj_t *s_test_screens[PAGE_COUNT], *s_desktop_screen,
    *s_detail_screen;
static lv_obj_t *s_detail_title, *s_detail_body, *s_detail_hint;
LV_FONT_DECLARE(app_ui_font_14);
static size_t s_current_page_index;
static bool s_desktop_open, s_detail_open, s_retry_requested;
static bool s_home_preview_requested;
static lv_obj_t *s_time_label, *s_date_label, *s_home_network,
    *s_home_network_slash;
typedef struct {
  lv_obj_t *group, *depth, *time, *date, *hands[3], *arc;
  lv_point_precise_t points[3][2];
} ui_clock_t;
static ui_clock_t
    s_clocks[2]; // Same dial on home and idle, separate LVGL objects.
static lv_obj_t *s_home_digital;
// Only UI callers holding the LVGL lock use this ~1.8 KiB snapshot. Keeping
// it off the 4 KiB button/motion stacks avoids nested forecast copies.
static EXT_RAM_BSS_ATTR app_weather_t s_ui_weather;
static EXT_RAM_BSS_ATTR app_weather_t s_detail_weather;
static bool s_weather_detail_dirty = true, s_weather_detail_cached;
static char s_weather_detail_hour[14];
static unsigned s_weather_uv_group;
static bool s_weather_uv_page;
static motion_gesture_t s_weather_vertical;
static lv_obj_t *s_forecast_group, *s_forecast_strip, *s_forecast_range,
    *s_forecast_status;
static lv_obj_t *s_forecast_time[FORECAST_SLOTS], *s_forecast_temp[FORECAST_SLOTS],
    *s_forecast_condition[FORECAST_SLOTS];
static lv_obj_t *s_forecast_shapes[FORECAST_SLOTS][10];
static unsigned s_forecast_indices[24], s_forecast_count, s_forecast_first;
static int32_t s_forecast_offset_mpx;
static char s_forecast_iso[17];
static const char *s_forecast_fresh;
static lv_obj_t *s_forecast_marks[12];
static lv_obj_t *s_metrics_group, *s_uv_title, *s_uv_value, *s_uv_level,
    *s_uv_max, *s_uv_marker, *s_uv_period,
    *s_aqi_title, *s_aqi_value, *s_aqi_info, *s_aqi_updated, *s_wind_title,
    *s_wind_value, *s_wind_info, *s_wind_gust,
    *s_humidity_value, *s_feels_value, *s_wind_needle, *s_wind_tip;
static lv_point_precise_t s_wind_points[2];
static lv_obj_t *s_weather_temperature, *s_weather_status, *s_weather_humidity;
static lv_obj_t *s_weather_shapes[10];
static lv_obj_t *s_tool_entry_hint, *s_tool_hold_hint;
static holo_robot_t s_voice_robot;
static lv_obj_t *s_voice_overlay, *s_voice_title, *s_voice_subtitle;
static lv_obj_t *s_mic_bars[5], *s_wait_dots[3], *s_error_options[2];
static size_t s_active_voice_state = NO_VOICE_STATE;
static unsigned s_error_selection, s_animation_frame;
static int s_displayed_mic_percent;
static lv_obj_t *s_wallpaper, *s_album_screen, *s_album_image, *s_album_info;
static lv_obj_t *s_album_name, *s_album_status, *s_album_hint, *s_idle_screen;
static bool s_album_open, s_album_info_visible = true, s_idle;
static int64_t s_last_activity;
static EXT_RAM_BSS_ATTR app_media_snapshot_t s_ui_media;
static app_media_appearance_t s_ui_appearance;
static uint32_t s_wallpaper_generation, s_album_generation;
// LVGL only sees UI-owned pixels. The worker's SD buffers never become image
// sources.
static uint8_t *s_wallpaper_pixels, *s_album_pixels;
static lv_image_dsc_t s_wallpaper_dsc = {
    .header = {.magic = LV_IMAGE_HEADER_MAGIC,
               .cf = LV_COLOR_FORMAT_RGB565,
               .w = 240,
               .h = 240,
               .stride = 480},
    .data_size = APP_MEDIA_IMAGE_BYTES};
static lv_image_dsc_t s_album_dsc = {.header = {.magic = LV_IMAGE_HEADER_MAGIC,
                                                .cf = LV_COLOR_FORMAT_RGB565,
                                                .w = 240,
                                                .h = 240,
                                                .stride = 480},
                                     .data_size = APP_MEDIA_IMAGE_BYTES};
static void refresh_media_ui(void);
static void render_weather_icon(lv_obj_t *shapes[10], int code, bool night,
                                int x, int y, int scale);

static void visible(lv_obj_t *object, bool show) {
  if (show == !lv_obj_has_flag(object, LV_OBJ_FLAG_HIDDEN))
    return;
  if (show)
    lv_obj_remove_flag(object, LV_OBJ_FLAG_HIDDEN);
  else
    lv_obj_add_flag(object, LV_OBJ_FLAG_HIDDEN);
}
static void set_text(lv_obj_t *label, const char *text) {
  if (strcmp(lv_label_get_text(label), text))
    lv_label_set_text(label, text);
}
static void set_textf(lv_obj_t *label, const char *format, ...)
    __attribute__((format(printf, 2, 3)));
static void set_textf(lv_obj_t *label, const char *format, ...) {
  // LVGL's small built-in formatter has LV_USE_FLOAT=0. Use libc here, and
  // avoid freeing/reallocating the label when the visible text is unchanged.
  char text[128];
  va_list args;
  va_start(args, format);
  vsnprintf(text, sizeof(text), format, args);
  va_end(args);
  set_text(label, text);
}

#define WALL_PARTICLES 40
typedef struct {
  lv_obj_t *group, *dots[WALL_PARTICLES];
} holo_wall_t;
static holo_wall_t s_animated;
static unsigned s_wall_frame;
static void create_animated_wall(lv_obj_t *parent) {
  holo_wall_t *w = &s_animated;
  w->group = holo_rect(parent, 0, 0, 240, 240, HOLO_INK, 0, false, 0);
  for (unsigned i = 0; i < WALL_PARTICLES; i++)
    w->dots[i] =
        holo_rect(w->group, 0, 0, 2, 2, HOLO_CREAM, LV_RADIUS_CIRCLE, true, 0);
  visible(w->group, false);
}
static void wallpaper_tick(lv_timer_t *timer) {
  (void)timer;
  unsigned mode = app_desktop_wallpaper_mode();
  if (!mode || s_idle || s_active_voice_state != NO_VOICE_STATE ||
      s_desktop_open || s_detail_open || s_album_open || s_current_page_index != 0)
    return;
  if (lv_obj_has_flag(s_animated.group, LV_OBJ_FLAG_HIDDEN))
    return;
  float t = (float)(++s_wall_frame) * 0.10f;
  for (unsigned i = 0; i < WALL_PARTICLES; i++) {
    float x = 0, y = 0;
    unsigned size = 2, color = HOLO_CREAM, opacity = 255;
    if (mode ==
        1) { // Stable star positions, staggered twinkle; no background haze.
      x = 12 + (i * 73u + 17u) % 216;
      y = 12 + (i * 97u + 29u) % 216;
      float pulse = 0.5f + 0.5f * sinf(t * (0.6f + (i % 7) * 0.13f) + i * 2.1f);
      opacity = (unsigned)(40 + 215 * pulse * pulse);
      size = i % 9 == 0 ? 3 : 2;
      color = i % 3 == 0 ? HOLO_MINT : HOLO_CREAM;
    } else if (mode == 2) {
      float a = (float)i * 6.2831853f / 32.0f + (i < 32 ? 0 : t * 0.45f);
      float r = i < 32 ? 84.0f : 48.0f + (i % 3) * 12.0f;
      x = 120 + cosf(a) * r;
      y = 125 + sinf(a) * r * 0.46f;
      size = i < 32 ? 2 : 5;
      opacity = i < 32 ? 130 : 255;
      color = i < 32 ? HOLO_APRICOT : HOLO_MINT;
    } else if (mode == 3) {
      float lift = 8 * sinf(t * 0.6f);
      if (i < 16) {
        float a = (float)i * 3.14159265f / 15.0f;
        x = 120 + cosf(a) * 52;
        y = 104 - sinf(a) * 36 + lift;
        size = 4;
      } else {
        unsigned n = i - 16;
        float row = n / 4, col = n % 4;
        x = 88 + col * 21 + 8 * sinf(t * 1.1f + row * 0.55f + col);
        y = 110 + row * 15 + lift;
        size = 3;
      }
      color = i < 16 ? HOLO_MINT : HOLO_CREAM;
      opacity = (unsigned)(130 + 125 * (0.5f + 0.5f * sinf(t + i * 0.25f)));
    } else {
      unsigned flower = i / 8, petal = i % 8;
      float a = petal * 0.785398f + t * (flower % 2 ? 0.1f : -0.1f);
      float pulse = 12 + 3 * sinf(t * 0.7f + flower);
      float cx = 35 + (flower * 61u) % 175, cy = 55 + (flower * 47u) % 140;
      x = cx + cosf(a) * pulse;
      y = cy + sinf(a) * pulse;
      size = petal == 0 ? 5 : 3;
      color = flower % 2 ? HOLO_CORAL : HOLO_APRICOT;
      opacity = 200 + (unsigned)(55 * (0.5f + 0.5f * sinf(t + flower)));
    }
    lv_obj_t *dot = s_animated.dots[i];
    lv_obj_set_pos(dot, (int)x, (int)y);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(dot, (lv_opa_t)opacity, 0);
  }
}

static void page_footer(lv_obj_t *screen, unsigned page) {
  // Five-pixel markers occupy the final five rows of the 240-pixel panel.
  for (unsigned i = 0; i < PAGE_COUNT; i++)
    holo_rect(screen, 90 + i * 18, 235, i == page ? 12 : 5, 5,
              i == page ? HOLO_APRICOT : HOLO_CREAM, 3, true, 0);
}
static void error_selection(void) {
  for (unsigned i = 0; i < 2; i++) {
    lv_obj_set_style_text_color(
        s_error_options[i],
        lv_color_hex(i == s_error_selection ? HOLO_APRICOT : HOLO_CREAM), 0);
    set_text(s_error_options[i],
                      i == 0 ? (s_error_selection == 0 ? "> 重试" : "重试")
                             : (s_error_selection == 1 ? "> 返回" : "返回"));
  }
}
static void hide_overlay_locked(void) {
  if (s_voice_overlay)
    visible(s_voice_overlay, false);
  s_active_voice_state = NO_VOICE_STATE;
  s_last_activity = esp_timer_get_time();
}
bool app_ui_note_activity(void) {
  s_last_activity = esp_timer_get_time();
  unsigned base = app_desktop_base_brightness();
  if (app_display_get_brightness() != base)
    app_display_set_brightness(base);
  bool woke = s_idle;
  if (s_idle) {
    s_idle = false;
    lv_screen_load(s_test_screens[s_current_page_index]);
  }
  return woke;
}
void app_ui_preview_home(void) { s_home_preview_requested = true; }
static void album_move(int direction) {
  if (s_active_voice_state != NO_VOICE_STATE)
    return;
  app_media_snapshot(&s_ui_media);
  if (s_ui_media.image_count) {
    unsigned next = (s_ui_media.album_index + s_ui_media.image_count +
                     (direction < 0 ? -1 : 1)) %
                    s_ui_media.image_count;
    app_media_select_image(next, false);
  }
}
void app_ui_open_album(void) {
  app_ui_note_activity();
  s_album_open = true;
  s_album_info_visible = true;
  app_desktop_visible(false);
  visible(s_album_info, true);
  refresh_media_ui();
  lv_screen_load(s_album_screen);
}
static void show_page(size_t page_index) {
  if (s_active_voice_state == APP_UI_VOICE_ERROR) {
    s_error_selection = 1 - s_error_selection;
    error_selection();
    return;
  }
  if (s_desktop_open || s_detail_open || s_album_open || s_idle ||
      page_index >= PAGE_COUNT || page_index == s_current_page_index ||
      s_active_voice_state != NO_VOICE_STATE)
    return;
  s_current_page_index = page_index;
  app_desktop_visible(false);
  // Immediate screen changes cannot leave a child/page animation pending.
  lv_screen_load(s_test_screens[s_current_page_index]);
}
void app_ui_show_next_page(void) {
  if (s_album_open) {
    album_move(1);
    return;
  }
  show_page((s_current_page_index + 1) % PAGE_COUNT);
}
void app_ui_show_previous_page(void) {
  if (s_album_open) {
    album_move(-1);
    return;
  }
  show_page((s_current_page_index + PAGE_COUNT - 1) % PAGE_COUNT);
}

static unsigned forecast_indices(const char *iso, unsigned indices[24]) {
  if (!iso || strlen(iso) != 16)
    return 0;
  // Include the current hour, then the next eleven; never invent missing data.
  char hour[17];
  memcpy(hour, iso, sizeof(hour));
  hour[14] = hour[15] = '0';
  unsigned count = app_weather_future_hours(&s_ui_weather, hour, indices);
  return count < 12 ? count : 12;
}
static void render_forecast_window(void) {
  unsigned first_index = (unsigned)s_forecast_offset_mpx / (FORECAST_COLUMN_PX * 1000);
  s_forecast_first = first_index;
  unsigned shown = s_forecast_count - first_index;
  if (shown > 4)
    shown = 4;
  const app_weather_hour_t *first =
      &s_detail_weather.hours[s_forecast_indices[first_index]];
  const app_weather_hour_t *last =
      &s_detail_weather.hours[s_forecast_indices[first_index + shown - 1]];
  set_textf(s_forecast_range, "%.5s %.2s时 - %.5s %.2s时",
                        first->time + 5, first->time + 11, last->time + 5,
                        last->time + 11);
  set_textf(s_forecast_status, "%u-%u/%u小时 · %s",
                        first_index + 1, first_index + shown, s_forecast_count,
                        s_forecast_count < 12 ? "数据不足，等待更新" : s_forecast_fresh);
  for (unsigned i = 0; i < 12; ++i) {
    visible(s_forecast_marks[i], i < s_forecast_count);
    bool selected = i >= first_index && i < first_index + shown;
    lv_obj_set_style_bg_opa(s_forecast_marks[i], selected ? LV_OPA_COVER : LV_OPA_40, 0);
  }
  for (unsigned i = 0; i < FORECAST_SLOTS; ++i) {
    unsigned index = first_index + i;
    bool available = index < s_forecast_count;
    visible(s_forecast_time[i], available);
    visible(s_forecast_temp[i], available);
    visible(s_forecast_condition[i], available);
    for (unsigned j = 0; j < 10; ++j)
      visible(s_forecast_shapes[i][j], false);
    if (!available)
      continue;
    const app_weather_hour_t *h =
        &s_detail_weather.hours[s_forecast_indices[index]];
    int x = (int)index * FORECAST_COLUMN_PX;
    lv_obj_set_x(s_forecast_time[i], x);
    lv_obj_set_x(s_forecast_temp[i], x);
    lv_obj_set_x(s_forecast_condition[i], x);
    unsigned hour = (unsigned)atoi(h->time + 11);
    if (!strncmp(h->time, s_forecast_iso, 13))
      set_text(s_forecast_time[i], "现在");
    else
      set_textf(s_forecast_time[i], "%u时", hour);
    set_textf(s_forecast_temp[i], "%d°", h->temperature_c);
    set_text(s_forecast_condition[i], app_weather_condition(h->code));
    render_weather_icon(s_forecast_shapes[i], h->code, hour < 6 || hour >= 18,
                        10 + x, 29, 55);
  }
}
static void render_forecast(const unsigned indices[24], unsigned count,
                            const char *iso, const char *fresh) {
  // Layout/scroll bounds must be current even when returning from the UV page.
  visible(s_forecast_group, true);
  s_forecast_count = count;
  memcpy(s_forecast_indices, indices, count * sizeof(*indices));
  memcpy(s_forecast_iso, iso, sizeof(s_forecast_iso));
  s_forecast_fresh = fresh;
  unsigned maximum = count > 4 ? count - 4 : 0;
  int32_t limit = (int32_t)maximum * FORECAST_COLUMN_PX * 1000;
  if (s_forecast_offset_mpx > limit)
    s_forecast_offset_mpx = limit;
  lv_obj_set_width(s_forecast_strip, 228 + (int)maximum * FORECAST_COLUMN_PX);
  render_forecast_window();
  lv_obj_update_layout(s_forecast_group);
  lv_obj_scroll_to_x(s_forecast_group, s_forecast_offset_mpx / 1000, LV_ANIM_OFF);
}
static int forecast_speed(int tilt) {
  int magnitude = abs(tilt);
  if (magnitude <= FORECAST_DEADZONE_MG)
    return 0;
  if (magnitude > FORECAST_FULL_TILT_MG)
    magnitude = FORECAST_FULL_TILT_MG;
  int speed = FORECAST_MIN_SPEED +
      (magnitude - FORECAST_DEADZONE_MG) * (FORECAST_MAX_SPEED - FORECAST_MIN_SPEED) /
      (FORECAST_FULL_TILT_MG - FORECAST_DEADZONE_MG);
  return tilt < 0 ? -speed : speed;
}
static void forecast_scroll_tick(int tilt, uint32_t elapsed_ms) {
  if (!s_forecast_count || !elapsed_ms)
    return;
  int speed = forecast_speed(tilt);
  if (!speed)
    return;
  // Keep integration fractional at slow speeds, and cap catch-up after a slow
  // frame. Invalid/stale sensor samples never move the timeline.
  if (elapsed_ms > 100)
    elapsed_ms = 100;
  int32_t next = s_forecast_offset_mpx + speed * (int32_t)elapsed_ms;
  unsigned maximum = s_forecast_count > 4 ? s_forecast_count - 4 : 0;
  int32_t limit = (int32_t)maximum * FORECAST_COLUMN_PX * 1000;
  if (next < 0) next = 0;
  if (next > limit) next = limit;
  if (next == s_forecast_offset_mpx)
    return;
  s_forecast_offset_mpx = next;
  unsigned first = (unsigned)next / (FORECAST_COLUMN_PX * 1000);
  if (first != s_forecast_first)
    render_forecast_window();
  int32_t pixel = next / 1000;
  if (pixel != lv_obj_get_scroll_x(s_forecast_group)) {
    lv_obj_update_layout(s_forecast_group);
    lv_obj_scroll_to_x(s_forecast_group, pixel, LV_ANIM_OFF);
  }
  s_last_activity = esp_timer_get_time();
}

static unsigned uv_period_count(char periods[192]) {
  unsigned count = 1;
  for (const char *p = periods; *p; ++p) if (*p == '\n') ++count;
  return count;
}
static void render_weather_metrics(const char *day, const char *fresh) {
  bool cached = strcmp(fresh, "逐时预报") != 0;
  set_text(s_uv_title, cached ? "紫外线 缓存" : "紫外线 当前");
  if (s_ui_weather.current_uv_available) {
    float uv = s_ui_weather.current_uv;
    set_textf(s_uv_value, "%.1f", (double)uv);
    set_text(s_uv_level, app_weather_uv_level(uv));
    unsigned color = uv < 3 ? HOLO_MINT : uv < 6 ? HOLO_APRICOT : HOLO_CORAL;
    lv_obj_set_style_text_color(s_uv_value, lv_color_hex(color), 0);
    lv_obj_set_x(s_uv_marker, 132 + (int)lroundf(fminf(uv, 11) / 11 * 92));
    visible(s_uv_marker, true);
  } else {
    set_text(s_uv_value, "--");
    set_text(s_uv_level, "暂无");
    visible(s_uv_marker, false);
  }
  if (s_ui_weather.uv_available && !strcmp(s_ui_weather.uv_day, day))
    set_textf(s_uv_max, "最高%.1f", (double)s_ui_weather.uv_max);
  else set_text(s_uv_max, "最高--");

  const app_air_quality_t *air = &s_ui_weather.air;
  bool same_day = air->available && !strncmp(air->updated_at, day, 10);
  set_text(s_aqi_title, s_ui_weather.air_cached || cached ? "US AQI 缓存" : "空气 US AQI");
  if (same_day) {
    lv_obj_set_style_text_font(s_aqi_info, &lv_font_montserrat_12, 0);
    set_textf(s_aqi_value, "%d", air->us_aqi);
    // Unit and update time get their own bounded line; never wrap into it.
    set_textf(s_aqi_info, "PM2.5 %.0f", (double)air->pm25);
    set_textf(s_aqi_updated, "ug/m3 %.5s", air->updated_at + 11);
    lv_obj_set_style_text_color(s_aqi_value, lv_color_hex(air->us_aqi <= 50 ? HOLO_MINT :
                                                       air->us_aqi <= 100 ? HOLO_APRICOT : HOLO_CORAL), 0);
  } else {
    lv_obj_set_style_text_font(s_aqi_info, &app_ui_font_14, 0);
    set_text(s_aqi_value, "--");
    set_text(s_aqi_info, "暂无空气数据");
    set_text(s_aqi_updated, "");
  }
  if (s_ui_weather.wind_available) {
    set_textf(s_wind_title, "风力 · %s", app_weather_wind_direction(s_ui_weather.wind_degrees));
    set_textf(s_wind_value, "%u级", app_weather_wind_level(s_ui_weather.wind_kmh));
    set_textf(s_wind_info, "%.0f km/h", (double)s_ui_weather.wind_kmh);
    set_textf(s_wind_gust, "阵风 %.0f km/h", (double)s_ui_weather.gust_kmh);
    double a = (s_ui_weather.wind_degrees - 90) * 0.017453292519943295;
    s_wind_points[0] = (lv_point_precise_t){19, 19};
    s_wind_points[1] = (lv_point_precise_t){19 + (int)lround(cos(a) * 14),
                                           19 + (int)lround(sin(a) * 14)};
    lv_line_set_points(s_wind_needle, s_wind_points, 2);
    lv_obj_set_pos(s_wind_tip, 76 + (int)s_wind_points[1].x - 2,
                               162 + (int)s_wind_points[1].y - 2);
    visible(s_wind_needle, true);
    visible(s_wind_tip, true);
  } else {
    set_text(s_wind_title, "风力");
    set_text(s_wind_value, "--");
    set_text(s_wind_info, "暂无风力数据");
    set_text(s_wind_gust, "");
    visible(s_wind_needle, false);
    visible(s_wind_tip, false);
  }
  if (s_ui_weather.humidity_percent >= 0)
    set_textf(s_humidity_value, "%d%%", s_ui_weather.humidity_percent);
  else set_text(s_humidity_value, "--");
  set_textf(s_feels_value, "体感 %d°", s_ui_weather.apparent_c);
  char periods[192];
  app_weather_uv_periods(&s_ui_weather, day, periods, sizeof(periods));
  unsigned groups = uv_period_count(periods);
  s_weather_uv_group %= groups;
  char *line = periods;
  for (unsigned i = 0; i < s_weather_uv_group; ++i) line = strchr(line, '\n') + 1;
  char *end = strchr(line, '\n');
  if (end) *end = 0;
  if (*line >= '0' && *line <= '9') {
    lv_obj_set_style_text_font(s_uv_period, &lv_font_montserrat_12, 0);
    set_textf(s_uv_period, ">3 %s", line);
  } else {
    lv_obj_set_style_text_font(s_uv_period, &app_ui_font_14, 0);
    set_text(s_uv_period, !strcmp(line, "无大于3的时段") ? ">3 无" : ">3 数据不足");
  }
  // Usually one interval. If today's forecast has several, horizontal input
  // browses them; the group index stays inside this same UV card.
  if (groups > 1)
    set_textf(s_uv_title, cached ? "紫外线 缓存%u/%u" : "紫外线 %u/%u",
              s_weather_uv_group + 1, groups);
}

static void refresh_page_details(void) {
  if (!s_detail_open)
    return;
  app_network_status_t network;
  app_network_status_snapshot(&network);
  char body[512] = "";
  char forecast_title[32];
  bool forecast_visible = false, metrics_visible = false;
  const char *title;
  if (s_current_page_index == 1) {
    title = s_weather_uv_page ? "天气参数" : "未来12小时";
    unsigned age;
    if (app_network_weather_snapshot(&s_ui_weather, &age)) {
      time_t now = time(NULL);
      struct tm local;
      char iso[17] = "", day[11] = "";
      if (network.time_valid && localtime_r(&now, &local)) {
        strftime(iso, sizeof(iso), "%Y-%m-%dT%H:%M", &local);
        strftime(day, sizeof(day), "%Y-%m-%d", &local);
      }
      const char *fresh = age > 1200 || network.weather_error != ESP_OK
                              ? "缓存预报，等待更新"
                              : "逐时预报";
      bool cached = age > 1200 || network.weather_error != ESP_OK;
      // No hide/show cycle, label churn or icon reconstruction every second.
      // Keep a UI-owned snapshot for the five-slot scrolling window as well.
      if (!s_weather_detail_dirty && cached == s_weather_detail_cached &&
          !strncmp(iso, s_weather_detail_hour, 13) &&
          !memcmp(&s_detail_weather, &s_ui_weather, sizeof(s_detail_weather)))
        return;
      s_detail_weather = s_ui_weather;
      s_weather_detail_cached = cached;
      memcpy(s_weather_detail_hour, iso, 13);
      s_weather_detail_hour[13] = 0;
      s_weather_detail_dirty = false;
      if (s_weather_uv_page) {
        metrics_visible = true;
        render_weather_metrics(day, fresh);
      } else {
        unsigned indices[24];
        unsigned count = forecast_indices(iso, indices);
        if (count) {
          snprintf(forecast_title, sizeof(forecast_title), "未来%u小时", count);
          title = forecast_title;
          forecast_visible = true;
          render_forecast(indices, count, iso, fresh);
        } else
          snprintf(body, sizeof(body),
                   "未来逐时数据暂不可用\n等待后台校时和更新\n");
      }
    } else {
      s_weather_detail_dirty = true;
      snprintf(body, sizeof(body),
               "%s\n暂无天气数据\n%s\n%s\n联网校时后自动获取", APP_WEATHER_CITY,
               network.connected ? "已联网" : "等待联网",
               network.time_valid ? "时间有效" : "等待校时");
    }
  } else {
    title = "语音操作";
    snprintf(body, sizeof(body),
             "本地唤醒或按住说话\n松开发送\n回答时按键停止\n错误后可重新提问");
  }
  visible(s_metrics_group, metrics_visible);
  visible(s_forecast_group, forecast_visible);
  visible(s_forecast_range, forecast_visible);
  visible(s_forecast_status, forecast_visible);
  for (unsigned i = 0; i < 12; ++i)
    visible(s_forecast_marks[i], forecast_visible && i < s_forecast_count);
  visible(s_detail_body, !forecast_visible && !metrics_visible);
  set_text(s_detail_title, title);
  set_text(s_detail_body, body);
  set_text(s_detail_hint, "");
  visible(s_detail_hint, false);
}
bool app_ui_detail_motion(int direction) {
  if (!s_detail_open || s_current_page_index != 1 ||
      s_active_voice_state != NO_VOICE_STATE)
    return false;
  if (s_weather_uv_page) {
    char periods[192];
    unsigned age;
    time_t now = time(NULL);
    struct tm local;
    char day[11];
    if (app_network_weather_snapshot(&s_ui_weather, &age) &&
        localtime_r(&now, &local) &&
        strftime(day, sizeof(day), "%Y-%m-%d", &local)) {
      app_weather_uv_periods(&s_ui_weather, day, periods, sizeof(periods));
      unsigned groups = uv_period_count(periods);
      s_weather_uv_group =
          (s_weather_uv_group + groups + (direction < 0 ? -1 : 1)) % groups;
    }
  } else
    // The 33-ms timer consumes continuous tilt input.
    return true;
  s_weather_detail_dirty = true;
  refresh_page_details();
  return true;
}
static void detail_motion_tick(lv_timer_t *timer) {
  (void)timer;
  static uint32_t previous_ms;
  uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
  uint32_t elapsed_ms = previous_ms ? now_ms - previous_ms : 0;
  previous_ms = now_ms;
  if (s_active_voice_state == NO_VOICE_STATE &&
      (s_idle || (!s_desktop_open && !s_detail_open && !s_album_open &&
                  s_current_page_index == 0 && !app_desktop_home_style()))) {
    int x = 0, y = 0;
    if (!app_motion_sample(&x, &y))
      x = y = 0;
    int dx = -x / 100, dy = -y / 100;
    if (dx < -4)
      dx = -4;
    if (dx > 4)
      dx = 4;
    if (dy < -4)
      dy = -4;
    if (dy > 4)
      dy = 4;
    ui_clock_t *clock = &s_clocks[s_idle ? 1 : 0];
    lv_obj_set_style_translate_x(clock->depth, 3 + dx, 0);
    lv_obj_set_style_translate_y(clock->depth, 2 + dy, 0);
  }
  if (!s_detail_open || s_current_page_index != 1 ||
      s_active_voice_state != NO_VOICE_STATE)
    return;
  int x = 0, y = 0;
  bool valid = app_motion_sample(&x, &y);
  int direction = motion_gesture_step(&s_weather_vertical, y, now_ms, valid,
                                      abs(x) > MOTION_CENTER_MG ||
                                      app_button_click_pending());
  if (direction) {
    app_ui_note_activity();
    // A two-page carousel: either direction advances to the other page.
    s_weather_uv_page = !s_weather_uv_page;
    s_weather_detail_dirty = true;
    refresh_page_details();
  }
  if (!s_weather_uv_page && valid && abs(y) <= MOTION_CENTER_MG &&
      !app_button_click_pending() &&
      !lv_obj_has_flag(s_forecast_group, LV_OBJ_FLAG_HIDDEN))
    forecast_scroll_tick(x, elapsed_ms);
}
void app_ui_enter_page(void) {
  if (s_active_voice_state == APP_UI_VOICE_ERROR) {
    if (!app_motion_available()) {
      s_error_selection = 1 - s_error_selection;
      error_selection();
      return;
    }
    s_retry_requested = s_error_selection == 0;
    hide_overlay_locked();
    return;
  }
  if (s_album_open && s_active_voice_state == NO_VOICE_STATE) {
    s_album_info_visible = !s_album_info_visible;
    visible(s_album_info, s_album_info_visible);
    return;
  }
  if (s_detail_open && s_current_page_index == 1 &&
      s_active_voice_state == NO_VOICE_STATE) {
    s_weather_uv_page = !s_weather_uv_page;
    s_weather_detail_dirty = true;
    refresh_page_details();
    return;
  }
  if (s_desktop_open || s_detail_open || s_active_voice_state != NO_VOICE_STATE)
    return;
  // With no fresh motion sample, single-click cycles; hold confirms the page.
  if (!app_motion_available()) {
    show_page((s_current_page_index + 1) % PAGE_COUNT);
    return;
  }
  if (s_current_page_index == 3 || s_current_page_index == 0) {
    s_desktop_open = true;
    if (s_current_page_index == 0)
      app_desktop_open_clock();
    else
      app_desktop_open_root();
    app_desktop_visible(true);
    lv_screen_load(s_desktop_screen);
  } else {
    s_detail_open = true;
    s_forecast_offset_mpx = 0;
    s_forecast_count = 0;
    s_weather_detail_dirty = true;
    s_weather_uv_group = 0;
    s_weather_uv_page = false;
    s_weather_vertical = (motion_gesture_t){.require_stable_center = true};
    refresh_page_details();
    lv_screen_load(s_detail_screen);
  }
}
void app_ui_back_page(void) {
  if (s_active_voice_state == APP_UI_VOICE_ERROR) {
    s_retry_requested = false;
    hide_overlay_locked();
    return;
  }
  if (s_album_open) {
    s_album_open = false;
    s_desktop_open = true;
    app_desktop_visible(true);
    lv_screen_load(s_desktop_screen);
    return;
  }
  if (!s_desktop_open && !s_detail_open)
    return;
  s_desktop_open = s_detail_open = false;
  s_weather_detail_dirty = true;
  app_desktop_visible(false);
  lv_screen_load(s_test_screens[s_current_page_index]);
}
bool app_ui_voice_allowed(void) {
  return s_current_page_index == 2 && !s_desktop_open && !s_detail_open &&
         !s_album_open && !s_idle && s_active_voice_state == NO_VOICE_STATE;
}
bool app_ui_open_destination(const char *target) {
  if (!target || s_active_voice_state != NO_VOICE_STATE) return false;
  const char *allowed[] = {"home", "weather", "assistant", "tools", "forecast", "environment",
    "alarm", "tasks", "settings", "focus", "music", "ambient",
    "media", "wallpapers", "anniversaries", "album", "web"};
  bool valid = false;
  for (unsigned i = 0; i < sizeof(allowed) / sizeof(*allowed); ++i)
    if (!strcmp(target, allowed[i])) { valid = true; break; }
  if (!valid) return false;
  app_ui_note_activity();
  s_desktop_open = s_detail_open = s_album_open = false;
  app_desktop_visible(false);
  if (!strcmp(target, "home") || !strcmp(target, "weather") ||
      !strcmp(target, "assistant") || !strcmp(target, "tools")) {
    s_current_page_index = !strcmp(target, "home") ? 0 :
        !strcmp(target, "weather") ? 1 : !strcmp(target, "assistant") ? 2 : 3;
    lv_screen_load(s_test_screens[s_current_page_index]);
  } else if (!strcmp(target, "forecast") || !strcmp(target, "environment")) {
    s_current_page_index = 1;
    s_detail_open = true;
    s_forecast_offset_mpx = s_forecast_count = s_weather_uv_group = 0;
    s_weather_uv_page = !strcmp(target, "environment");
    s_weather_detail_dirty = true;
    s_weather_vertical = (motion_gesture_t){.require_stable_center = true};
    refresh_page_details();
    lv_screen_load(s_detail_screen);
  } else if (!strcmp(target, "album")) {
    s_current_page_index = 3;
    app_desktop_open_tool(3);
    app_ui_open_album();
  } else {
    s_current_page_index = !strcmp(target, "alarm") ? 0 : 3;
    app_desktop_open_voice_menu(target);
    s_desktop_open = true;
    app_desktop_visible(true);
    lv_screen_load(s_desktop_screen);
  }
  return true;
}
bool app_ui_wake_voice(void) {
  if (s_active_voice_state != NO_VOICE_STATE &&
      s_active_voice_state != APP_UI_VOICE_ERROR &&
      s_active_voice_state != APP_UI_VOICE_CANCELLED)
    return false;
  // A completed failure/cancel screen leaves audio ownership idle. The
  // coordinator checks ownership before calling this under the LVGL lock.
  hide_overlay_locked();
  s_retry_requested = false;
  app_ui_note_activity();
  // Wake is global, including the settings page used to enable it. Leaving a
  // Manual button rules are unchanged.
  s_desktop_open = s_detail_open = s_album_open = false;
  s_weather_detail_dirty = true;
  app_desktop_visible(false);
  s_current_page_index = 2;
  lv_screen_load(s_test_screens[2]);
  return true;
}
bool app_ui_take_retry(void) {
  bool retry = s_retry_requested;
  s_retry_requested = false;
  return retry;
}
bool app_ui_handle_long_press(void) {
  if (s_active_voice_state == APP_UI_VOICE_ERROR) {
    s_retry_requested = s_error_selection == 0;
    hide_overlay_locked();
    return true;
  }
  if (s_album_open && !app_motion_available()) {
    album_move(1);
    return true;
  }
  if (s_desktop_open || s_detail_open || s_album_open)
    return true;
  if (s_current_page_index == 3) {
    s_desktop_open = true;
    app_desktop_open_settings();
    app_desktop_visible(true);
    lv_screen_load(s_desktop_screen);
    return true;
  }
  if (s_current_page_index == 2)
    return false;
  if (!app_motion_available()) {
    if (s_current_page_index == 0) {
      s_desktop_open = true;
      app_desktop_open_clock();
      app_desktop_visible(true);
      lv_screen_load(s_desktop_screen);
    } else {
      s_detail_open = true;
      refresh_page_details();
      lv_screen_load(s_detail_screen);
    }
  }
  return true;
}
static void create_clock(lv_obj_t *screen, ui_clock_t *clock) {
  // Reference design: luminous sparse ring, four cardinal markers and
  // warm hands. Every unpainted pixel remains black.
  clock->group = holo_rect(screen, 0, 0, 240, 232, HOLO_INK, 0, false, 0);
  screen = clock->group;
  holo_rect(screen, 20, 27, 200, 200, HOLO_APRICOT, LV_RADIUS_CIRCLE, false, 3);
  holo_rect(screen, 117, 37, 6, 15, HOLO_CREAM, 1, true, 0);
  holo_rect(screen, 117, 202, 6, 15, HOLO_CREAM, 1, true, 0);
  holo_rect(screen, 30, 124, 15, 6, HOLO_CREAM, 1, true, 0);
  holo_rect(screen, 195, 124, 15, 6, HOLO_CREAM, 1, true, 0);
  clock->arc = lv_arc_create(screen);
  lv_obj_remove_style_all(clock->arc);
  lv_obj_set_pos(clock->arc, 18, 25);
  lv_obj_set_size(clock->arc, 204, 204);
  lv_obj_remove_flag(clock->arc, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_arc_width(clock->arc, 7, LV_PART_INDICATOR);
  lv_obj_set_style_arc_color(clock->arc, lv_color_hex(HOLO_CORAL),
                             LV_PART_INDICATOR);
  lv_arc_set_rotation(clock->arc, 270);
  lv_arc_set_bg_angles(clock->arc, 0, 360);
  lv_arc_set_angles(clock->arc, 0, 45);
  // Keep the numeric plane behind the hands, but fully lit for daylight.
  // Small opposite-motion parallax creates a visual depth cue.
  clock->depth = holo_rect(screen, 0, 0, 240, 232, HOLO_INK, 0, false, 0);
  clock->time = holo_label(clock->depth, "--:--", &lv_font_montserrat_40,
                           0xFFFFFF, 28, 65, 184, LV_TEXT_ALIGN_CENTER);
  clock->date = holo_label(clock->depth, "等待校时", &app_ui_font_20,
                           HOLO_CREAM, 28, 166, 184, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_style_text_opa(clock->time, LV_OPA_COVER, 0);
  lv_obj_set_style_text_opa(clock->date, LV_OPA_COVER, 0);
  for (unsigned i = 0; i < 3; ++i) {
    clock->hands[i] = lv_line_create(screen);
    lv_obj_remove_style_all(clock->hands[i]);
    lv_obj_set_pos(clock->hands[i], 0, 0);
    lv_obj_set_style_line_width(clock->hands[i],
                                i == 0   ? 7
                                : i == 1 ? 5
                                         : 1,
                                0);
    lv_obj_set_style_line_color(clock->hands[i],
                                lv_color_hex(i == 0   ? HOLO_CREAM
                                             : i == 1 ? HOLO_CORAL
                                                      : HOLO_MINT),
                                0);
    lv_obj_set_style_line_rounded(clock->hands[i], true, 0);
    visible(clock->hands[i], false);
  }
  holo_rect(screen, 113, 120, 14, 14, HOLO_CORAL, LV_RADIUS_CIRCLE, true, 0);
  holo_rect(screen, 116, 123, 8, 8, HOLO_CREAM, LV_RADIUS_CIRCLE, true, 0);
}
static void create_page(lv_obj_t *screen, unsigned index) {
  holo_surface(screen);
  if (index == 0) {
    s_wallpaper = lv_image_create(screen);
    if (s_wallpaper_dsc.data)
      lv_image_set_src(s_wallpaper, &s_wallpaper_dsc);
    lv_obj_set_style_image_opa(s_wallpaper, LV_OPA_COVER, 0);
    visible(s_wallpaper, false);
    create_animated_wall(screen);
    create_clock(screen, &s_clocks[0]);
    s_home_digital = holo_rect(screen, 0, 0, 240, 232, HOLO_INK, 0, false, 0);
  }
  // Clock and weather content identify themselves without a duplicate title.
  if (index >= 2)
    holo_label(screen, PAGE_TITLES[index], &app_ui_font_20, HOLO_CREAM, 20, 20,
               200, LV_TEXT_ALIGN_CENTER);
  if (index == 0) {
    s_home_network = holo_label(screen, LV_SYMBOL_WIFI, &lv_font_montserrat_20,
                                HOLO_CORAL, 196, 20, 24, LV_TEXT_ALIGN_CENTER);
    s_home_network_slash =
        holo_label(screen, "\\", &lv_font_montserrat_20, HOLO_CORAL, 196, 20,
                   24, LV_TEXT_ALIGN_CENTER);
    // Optional wallpaper + digital time home; the default is the shared dial.
    s_time_label = holo_label(s_home_digital, "--:--", &app_ui_font_digits_56,
                              HOLO_CREAM, 20, 75, 200, LV_TEXT_ALIGN_CENTER);
    s_date_label = holo_label(s_home_digital, "等待校时", &app_ui_font_16,
                              HOLO_APRICOT, 20, 139, 200, LV_TEXT_ALIGN_CENTER);
  } else if (index == 1) {
    for (unsigned i = 0; i < 10; i++)
      s_weather_shapes[i] =
          holo_rect(screen, 88, 50, 12, 12, HOLO_APRICOT, 3, true, 0);
    s_weather_temperature =
        holo_label(screen, "--°", &app_ui_font_digits_56, HOLO_CREAM, 20, 110,
                   200, LV_TEXT_ALIGN_CENTER);
    s_weather_humidity =
        holo_label(screen, "湿度 --", &app_ui_font_16, HOLO_MINT, 20, 170, 200,
                   LV_TEXT_ALIGN_CENTER);
    s_weather_status =
        holo_label(screen, "暂无天气", &app_ui_font_16, HOLO_APRICOT, 20, 190,
                   200, LV_TEXT_ALIGN_CENTER);
    for (unsigned i = 0; i < 10; i++)
      visible(s_weather_shapes[i], false);
  } else if (index == 2) {
    holo_robot_create(screen, 72, 61, 2);
    holo_label(screen, "按住说话", &app_ui_font_20, HOLO_APRICOT, 20, 166, 200,
               LV_TEXT_ALIGN_CENTER);
    holo_label(screen, "松开发送", &app_ui_font_16, HOLO_CREAM, 20, 190, 200,
               LV_TEXT_ALIGN_CENTER);
  } else {
    holo_tool_icon(screen, 0, 80, 74);
    s_tool_entry_hint =
        holo_label(screen, "专注 · 任务 · 媒体", &app_ui_font_20, HOLO_APRICOT,
                   20, 153, 200, LV_TEXT_ALIGN_CENTER);
    s_tool_hold_hint =
        holo_label(screen, "专注 · 学习", &app_ui_font_16, HOLO_CREAM, 20,
                   181, 200, LV_TEXT_ALIGN_CENTER);
  }
  page_footer(screen, index);
}
static void create_voice_overlay(void) {
  s_voice_overlay = lv_obj_create(lv_layer_top());
  holo_surface(s_voice_overlay);
  lv_obj_set_size(s_voice_overlay, 240, 240);
  lv_obj_set_pos(s_voice_overlay, 0, 0);
  s_voice_title = holo_label(s_voice_overlay, "", &app_ui_font_20, HOLO_APRICOT,
                             20, 20, 200, LV_TEXT_ALIGN_CENTER);
  s_voice_robot = holo_robot_create(s_voice_overlay, 72, 53, 2);
  for (unsigned i = 0; i < 5; i++)
    s_mic_bars[i] = holo_rect(s_voice_overlay, 84 + i * 15, 150, 9, 5,
                              HOLO_MINT, 4, true, 0);
  for (unsigned i = 0; i < 3; i++)
    s_wait_dots[i] = holo_rect(s_voice_overlay, 100 + i * 18, 151, 7, 7,
                               HOLO_APRICOT, 4, true, 0);
  s_voice_subtitle = holo_label(s_voice_overlay, "", &app_ui_font_16,
                                HOLO_CREAM, 20, 173, 200, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_height(s_voice_subtitle, 40);
  lv_label_set_long_mode(s_voice_subtitle, LV_LABEL_LONG_MODE_WRAP);
  for (unsigned i = 0; i < 2; i++)
    s_error_options[i] =
        holo_label(s_voice_overlay, "", &app_ui_font_20, HOLO_CREAM,
                   25 + i * 100, 196, 90, LV_TEXT_ALIGN_CENTER);
  visible(s_voice_overlay, false);
}
static void show_voice_state(size_t state, const char *message) {
  // 提醒/语音优先：释放游戏画面和输入，避免遮罩后仍射击或累计失球。
  if (app_games_active()) app_desktop_back();
  s_active_voice_state = state;
  s_animation_frame = 0;
  s_displayed_mic_percent = 0;
  atomic_store(&s_mic_level, 0);
  s_error_selection = 0;
  set_text(s_voice_title, VOICE_STATES[state]);
  set_text(s_voice_subtitle,
                    message                           ? message
                    : state == APP_UI_VOICE_LISTENING ? "请开始说话"
                    : state == APP_UI_VOICE_THINKING  ? "正在处理语音"
                    : state == APP_UI_VOICE_SPEAKING  ? "按键可停止回答"
                    : state == APP_UI_VOICE_ERROR     ? "请重试或返回"
                                                      : "");
  lv_obj_set_pos(s_voice_subtitle, 20, state == APP_UI_VOICE_ERROR ? 161 : 173);
  lv_obj_set_height(s_voice_subtitle, state == APP_UI_VOICE_ERROR ? 30 : 40);
  lv_label_set_long_mode(s_voice_subtitle, state == APP_UI_VOICE_ERROR
                                               ? LV_LABEL_LONG_MODE_DOTS
                                               : LV_LABEL_LONG_MODE_WRAP);
  holo_robot_pose(&s_voice_robot, 0, state == APP_UI_VOICE_ERROR);
  for (unsigned i = 0; i < 5; i++)
    visible(s_mic_bars[i], state == APP_UI_VOICE_LISTENING);
  for (unsigned i = 0; i < 3; i++)
    visible(s_wait_dots[i], state == APP_UI_VOICE_THINKING);
  for (unsigned i = 0; i < 2; i++)
    visible(s_error_options[i], state == APP_UI_VOICE_ERROR);
  if (state == APP_UI_VOICE_ERROR)
    error_selection();
  visible(s_voice_overlay, true);
  APP_LOG_DETAIL(TAG, "显示语音状态: %s", VOICE_STATES[state]);
}
void app_ui_show_voice_state(app_ui_voice_state_t state, const char *message) {
  if ((unsigned)state >= NO_VOICE_STATE || !lvgl_port_lock(0))
    return;
  app_ui_note_activity();
  show_voice_state(state, message);
  lvgl_port_unlock();
}
void app_ui_hide_voice_state(void) {
  if (!lvgl_port_lock(0))
    return;
  hide_overlay_locked();
  lvgl_port_unlock();
}
void app_ui_set_mic_level(uint32_t level) { atomic_store(&s_mic_level, level); }
void app_ui_set_wifi_connected(bool connected) {
  atomic_store(&s_wifi_connected, connected);
}

static void update_mic_ui(lv_timer_t *timer) {
  (void)timer;
  // Runs under LVGL's lock, never from the microphone/AEC worker. Pending PCM
  // continues recording even while the cancelled network jobs are closing.
  if (app_acoustic_pending() &&
      (s_active_voice_state == APP_UI_VOICE_SPEAKING ||
       s_active_voice_state == APP_UI_VOICE_THINKING))
    show_voice_state(APP_UI_VOICE_LISTENING, "正在聆听");
  if (s_active_voice_state == APP_UI_VOICE_LISTENING) {
    unsigned level = atomic_load(&s_mic_level);
    int percent = level >= MIC_UI_FULL_SCALE ? 100
                  : level <= MIC_UI_NOISE_FLOOR
                      ? 0
                      : (int)(((uint64_t)(level - MIC_UI_NOISE_FLOOR) * 100) /
                              (MIC_UI_FULL_SCALE - MIC_UI_NOISE_FLOOR));
    s_displayed_mic_percent = (s_displayed_mic_percent * 2 + percent) / 3;
    // Real microphone level only; no random/synthetic voice waveform.
    static const unsigned weights[5] = {55, 80, 100, 80, 55};
    for (unsigned i = 0; i < 5; i++) {
      unsigned height = 5 + s_displayed_mic_percent * weights[i] / 350;
      lv_obj_set_height(s_mic_bars[i], height);
      lv_obj_set_y(s_mic_bars[i], 164 - height);
    }
  } else if (s_active_voice_state == APP_UI_VOICE_SPEAKING ||
             s_active_voice_state == APP_UI_VOICE_REMINDER) {
    if (++s_animation_frame % 3 == 0)
      holo_robot_pose(&s_voice_robot, s_animation_frame / 3, false);
  } else if (s_active_voice_state == APP_UI_VOICE_THINKING) {
    ++s_animation_frame;
    for (unsigned i = 0; i < 3; i++)
      visible(s_wait_dots[i], i == (s_animation_frame / 4) % 3);
  }
}
static void weather_shape(lv_obj_t *object, int x, int y, int w, int h,
                          unsigned color, int radius, int scale) {
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, w * scale / 100, h * scale / 100);
  lv_obj_set_style_radius(
      object, radius == LV_RADIUS_CIRCLE ? radius : radius * scale / 100, 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  visible(object, true);
}
static void render_weather_icon(lv_obj_t *shapes[10], int code, bool night,
                                int x, int y, int scale) {
  for (unsigned i = 0; i < 10; i++)
    visible(shapes[i], false);
  if (code <= 2) {
    weather_shape(shapes[0], x + 11 * scale / 100, y + 9 * scale / 100, 36, 36,
                  HOLO_APRICOT, LV_RADIUS_CIRCLE, scale);
    if (night) {
      weather_shape(shapes[1], x + 23 * scale / 100, y + 3 * scale / 100, 30,
                    30, HOLO_INK, LV_RADIUS_CIRCLE, scale);
      weather_shape(shapes[2], x + 48 * scale / 100, y + 8 * scale / 100, 5, 5,
                    HOLO_CREAM, 1, scale);
    } else {
      weather_shape(shapes[1], x + 27 * scale / 100, y, 4, 7, HOLO_CREAM, 2,
                    scale);
      weather_shape(shapes[2], x + 27 * scale / 100, y + 47 * scale / 100, 4, 7,
                    HOLO_CREAM, 2, scale);
      weather_shape(shapes[3], x, y + 25 * scale / 100, 7, 4, HOLO_CREAM, 2,
                    scale);
      weather_shape(shapes[4], x + 51 * scale / 100, y + 25 * scale / 100, 7, 4,
                    HOLO_CREAM, 2, scale);
    }
  }
  if (code > 0) {
    weather_shape(shapes[5], x, y + 28 * scale / 100, 60, 22, HOLO_CREAM, 11,
                  scale);
    weather_shape(shapes[6], x + 8 * scale / 100, y + 15 * scale / 100, 28, 28,
                  HOLO_CREAM, LV_RADIUS_CIRCLE, scale);
    weather_shape(shapes[7], x + 27 * scale / 100, y + 21 * scale / 100, 26, 25,
                  HOLO_CREAM, LV_RADIUS_CIRCLE, scale);
    if (code >= 51) {
      bool snow = (code >= 71 && code <= 77) || code == 85 || code == 86;
      weather_shape(shapes[8], x + 18 * scale / 100, y + 52 * scale / 100,
                    snow ? 7 : 4, snow ? 7 : 9, snow ? HOLO_CREAM : HOLO_MINT,
                    2, scale);
      weather_shape(shapes[9], x + 42 * scale / 100, y + 52 * scale / 100,
                    snow ? 7 : 4, snow ? 7 : 9, snow ? HOLO_CREAM : HOLO_MINT,
                    2, scale);
    }
  }
}
static void render_weather(const app_weather_t *weather) {
  unsigned hour = (unsigned)atoi(weather->updated_at + 11);
  render_weather_icon(s_weather_shapes, weather->code, hour < 6 || hour >= 18,
                      91, 47, 100);
  set_textf(s_weather_temperature, "%d°", weather->temperature_c);
  if (weather->humidity_percent >= 0)
    set_textf(s_weather_humidity, "湿度 %d%%",
                          weather->humidity_percent);
  else
    set_text(s_weather_humidity, "湿度 暂无数据");
}
static void refresh_media_ui(void) {
  if (s_home_preview_requested && s_active_voice_state == NO_VOICE_STATE) {
    s_home_preview_requested = false;
    app_ui_note_activity();
    s_desktop_open = s_detail_open = s_album_open = false;
    s_current_page_index = 0;
    app_desktop_visible(false);
    lv_screen_load(s_test_screens[0]);
  }
  app_media_snapshot(&s_ui_media);
  app_media_get_appearance(&s_ui_appearance);
  if (app_media_copy_image(false, s_wallpaper_pixels, APP_MEDIA_IMAGE_BYTES,
                           &s_wallpaper_generation)) {
    // Native, aligned RGB565 is drawn directly, without a full-frame decoder
    // cache.
    lv_obj_invalidate(s_wallpaper);
  }
  if (app_media_copy_image(true, s_album_pixels, APP_MEDIA_IMAGE_BYTES,
                           &s_album_generation)) {
    lv_obj_invalidate(s_album_image);
  }
  bool digital_home = app_desktop_home_style() == 1;
  bool dynamic = app_desktop_wallpaper_mode() != 0;
  visible(s_animated.group, digital_home && dynamic);
  visible(s_wallpaper, digital_home && !dynamic && s_ui_media.image_count &&
                           s_wallpaper_generation);
  visible(s_home_digital, digital_home);
  visible(s_clocks[0].group, !digital_home);
  visible(s_album_image, s_ui_media.image_count && s_album_generation);
  set_text(
      s_album_name,
      s_ui_media.image_count && s_ui_media.album_index < s_ui_media.image_count
          ? app_media_file_label(s_ui_media.images[s_ui_media.album_index])
          : "暂无图片");
  if (!s_album_pixels)
    set_text(s_album_status, "图片缓存内存不足");
  else if (s_ui_media.image_count)
    set_textf(s_album_status, "%u/%u", s_ui_media.album_index + 1,
                          s_ui_media.image_count);
  else
    set_text(s_album_status,
                      s_ui_media.ready ? "请用网页上传图片" : "SD卡不可用");
  set_text(s_album_hint, "");
  visible(s_album_hint, false);

  int64_t monotonic = esp_timer_get_time();
  if (s_idle && !s_ui_appearance.idle_seconds)
    app_ui_note_activity();
  bool root = !s_desktop_open && !s_detail_open && !s_album_open &&
              s_active_voice_state == NO_VOICE_STATE;
  if (!s_idle && root && s_ui_appearance.idle_seconds &&
      monotonic - s_last_activity >=
          (int64_t)s_ui_appearance.idle_seconds * 1000000) {
    s_idle = true;
    lv_screen_load(s_idle_screen);
  }

  app_network_status_t network;
  app_network_status_snapshot(&network);
  time_t utc = time(NULL);
  struct tm local;
  bool valid_time =
      network.time_valid && localtime_r(&utc, &local) && local.tm_year >= 124;
  unsigned effective = app_desktop_base_brightness();
  // Input holds normal brightness for 15 seconds; menus/voice never get dimmed
  // mid-use.
  if (valid_time && s_ui_appearance.night_enabled && root &&
      monotonic - s_last_activity >= 15000000) {
    unsigned minute = (unsigned)local.tm_hour * 60 + (unsigned)local.tm_min;
    unsigned start = s_ui_appearance.night_start_minute,
             end = s_ui_appearance.night_end_minute;
    bool night = start < end ? minute >= start && minute < end
                             : minute >= start || minute < end;
    if (night && effective > s_ui_appearance.night_brightness)
      effective = s_ui_appearance.night_brightness;
  }
  if (app_display_get_brightness() != effective)
    app_display_set_brightness(effective);
}
static void update_dial(ui_clock_t *clock, const struct tm *local,
                        const char *status) {
  if (!local) {
    set_text(clock->time, "--:--");
    set_text(clock->date, status);
    for (unsigned i = 0; i < 3; ++i)
      visible(clock->hands[i], false);
    visible(clock->arc, false);
    return;
  }
  static const char *weekdays[] = {"周日", "周一", "周二", "周三",
                                   "周四", "周五", "周六"};
  char time_text[6];
  strftime(time_text, sizeof(time_text), "%H:%M", local);
  set_text(clock->time, time_text);
  set_textf(clock->date, "%02d/%02d %s", local->tm_mon + 1,
                        local->tm_mday, weekdays[local->tm_wday]);
  double angles[3] = {(local->tm_hour % 12 + local->tm_min / 60.0) * 30,
                      (local->tm_min + local->tm_sec / 60.0) * 6,
                      local->tm_sec * 6.0};
  const int lengths[] = {49, 76, 84};
  for (unsigned i = 0; i < 3; ++i) {
    double radians = (angles[i] - 90) * 0.017453292519943295;
    clock->points[i][0] = (lv_point_precise_t){120, 127};
    clock->points[i][1] =
        (lv_point_precise_t){120 + (int)lround(cos(radians) * lengths[i]),
                             127 + (int)lround(sin(radians) * lengths[i])};
    lv_line_set_points(clock->hands[i], clock->points[i], 2);
    visible(clock->hands[i], true);
  }
  lv_arc_set_angles(clock->arc, 0, local->tm_min * 6 + local->tm_sec / 10);
  visible(clock->arc, local->tm_min || local->tm_sec >= 10);
}
static void update_home_time(lv_timer_t *timer) {
  (void)timer;
  refresh_media_ui();
  set_text(s_tool_entry_hint, "专注 · 任务 · 媒体");
  set_text(s_tool_hold_hint, "番茄钟 · 词卡 · 媒体");
  refresh_page_details();
  app_network_status_t network;
  app_network_status_snapshot(&network);
  lv_obj_set_style_text_color(
      s_home_network, lv_color_hex(network.connected ? HOLO_MINT : HOLO_CORAL),
      0);
  lv_obj_set_style_text_opa(s_home_network,
                            network.connected ? LV_OPA_COVER : LV_OPA_40, 0);
  visible(s_home_network_slash, !network.connected);
  unsigned age = 0;
  static struct { int temperature_c, humidity_percent, code; char updated_at[32]; } displayed_weather;
  static bool weather_drawn;
  if (app_network_weather_snapshot(&s_ui_weather, &age)) {
    if (!weather_drawn ||
        s_ui_weather.temperature_c != displayed_weather.temperature_c ||
        s_ui_weather.humidity_percent != displayed_weather.humidity_percent ||
        s_ui_weather.code != displayed_weather.code ||
        strcmp(s_ui_weather.updated_at, displayed_weather.updated_at)) {
      render_weather(&s_ui_weather);
      displayed_weather.temperature_c = s_ui_weather.temperature_c;
    displayed_weather.humidity_percent = s_ui_weather.humidity_percent;
    displayed_weather.code = s_ui_weather.code;
    snprintf(displayed_weather.updated_at, sizeof(displayed_weather.updated_at), "%s", s_ui_weather.updated_at);
      weather_drawn = true;
    }
    set_text(s_weather_status,
                      network.weather_error != ESP_OK || age > 1200
                          ? "未更新"
                          : s_ui_weather.condition);
  } else {
    set_text(s_weather_status,
                      !network.connected ? "等待联网"
                      : !network.time_valid
                          ? (network.time_syncing ? "正在校时" : "校时重试")
                      : network.weather_updating        ? "获取天气"
                      : network.weather_error != ESP_OK ? "天气重试"
                                                        : "暂无天气");
    // Owner uses weather_retry_seconds for retry scheduling; UI never fetches.
    (void)network.weather_retry_seconds;
  }
  time_t now = time(NULL);
  struct tm local;
  if (!network.time_valid || localtime_r(&now, &local) == NULL ||
      local.tm_year < 124) {
    set_text(s_time_label, "--:--");
    if (!network.connected)
      set_text(s_date_label, "等待联网校时");
    else if (network.time_syncing)
      set_text(s_date_label, "正在校时");
    else
      set_textf(s_date_label, "校时重试 / %us",
                            network.time_retry_seconds);
    for (unsigned i = 0; i < 2; ++i)
      update_dial(&s_clocks[i], NULL, lv_label_get_text(s_date_label));
    return;
  }
  char clock[6];
  static const char *weekdays[] = {"周日", "周一", "周二", "周三",
                                   "周四", "周五", "周六"};
  strftime(clock, sizeof(clock), "%H:%M", &local);
  set_text(s_time_label, clock);
  set_textf(s_date_label, "%02d月%02d日  %s", local.tm_mon + 1,
                        local.tm_mday, weekdays[local.tm_wday]);
  for (unsigned i = 0; i < 2; ++i)
    update_dial(&s_clocks[i], &local, NULL);
}
static void discard_unpublished_images(void) {
  // Only before creating image widgets: published image data is retained
  // forever.
  free(s_wallpaper_pixels);
  free(s_album_pixels);
  s_wallpaper_pixels = s_album_pixels = NULL;
  s_wallpaper_dsc.data = s_album_dsc.data = NULL;
}
static void create_weather_metrics(void) {
  s_metrics_group = holo_rect(s_detail_screen, 0, 0, 240, 240, HOLO_INK, 0, false, 0);
  for (unsigned i = 0; i < 4; ++i)
    holo_rect(s_metrics_group, i % 2 ? 124 : 8, i / 2 ? 138 : 36,
              108, i / 2 ? 98 : 96, HOLO_APRICOT, 8, false, 1);
  s_aqi_title = holo_label(s_metrics_group, "空气 US AQI", &app_ui_font_14,
                           HOLO_CREAM, 14, 40, 96, LV_TEXT_ALIGN_CENTER);
  s_aqi_value = holo_label(s_metrics_group, "--", &lv_font_montserrat_32,
                           HOLO_MINT, 14, 58, 96, LV_TEXT_ALIGN_CENTER);
  s_aqi_info = holo_label(s_metrics_group, "", &app_ui_font_14,
                          HOLO_CREAM, 14, 96, 96, LV_TEXT_ALIGN_CENTER);
  s_aqi_updated = holo_label(s_metrics_group, "", &app_ui_font_14,
                           HOLO_APRICOT, 14, 114, 96, LV_TEXT_ALIGN_CENTER);
  lv_obj_set_style_text_font(s_aqi_updated, &lv_font_montserrat_12, 0);
  // Fixed single-line boxes keep numeric text clear of the PM/update rows.
  lv_obj_set_height(s_aqi_title, 17);
  lv_obj_set_height(s_aqi_value, 35);
  lv_obj_set_height(s_aqi_info, 17);
  lv_obj_set_height(s_aqi_updated, 15);
  s_uv_title = holo_label(s_metrics_group, "紫外线 当前", &app_ui_font_14,
                          HOLO_CREAM, 130, 40, 96, LV_TEXT_ALIGN_CENTER);
  s_uv_value = holo_label(s_metrics_group, "--", &lv_font_montserrat_32,
                          HOLO_APRICOT, 130, 58, 96, LV_TEXT_ALIGN_CENTER);
  s_uv_level = holo_label(s_metrics_group, "", &app_ui_font_14,
                          HOLO_CREAM, 130, 94, 40, LV_TEXT_ALIGN_LEFT);
  s_uv_max = holo_label(s_metrics_group, "", &app_ui_font_14,
                        HOLO_APRICOT, 164, 94, 62, LV_TEXT_ALIGN_RIGHT);
  s_uv_period = holo_label(s_metrics_group, "", &lv_font_montserrat_12,
                           HOLO_CREAM, 130, 112, 96, LV_TEXT_ALIGN_CENTER);
  const unsigned colors[] = {HOLO_MINT, 0xFFF06D, 0xFFAC60, 0xFF796C, 0xD895FF};
  for (unsigned i = 0; i < 5; ++i)
    holo_rect(s_metrics_group, 132 + (int)i * 19, 128, 18, 3, colors[i], 1, true, 0);
  s_uv_marker = holo_rect(s_metrics_group, 132, 127, 4, 4, 0xFFFFFF, 2, true, 0);
  s_wind_title = holo_label(s_metrics_group, "风力", &app_ui_font_14, HOLO_CREAM,
              14, 144, 96, LV_TEXT_ALIGN_LEFT);
  s_wind_value = holo_label(s_metrics_group, "--", &app_ui_font_20,
                            HOLO_CREAM, 14, 174, 54, LV_TEXT_ALIGN_CENTER);
  holo_rect(s_metrics_group, 76, 162, 38, 38, HOLO_APRICOT, LV_RADIUS_CIRCLE, false, 1);
  s_wind_needle = lv_line_create(s_metrics_group);
  lv_obj_remove_style_all(s_wind_needle);
  lv_obj_set_pos(s_wind_needle, 76, 162);
  lv_obj_set_style_line_width(s_wind_needle, 2, 0);
  lv_obj_set_style_line_color(s_wind_needle, lv_color_hex(HOLO_CREAM), 0);
  s_wind_tip = holo_rect(s_metrics_group, 76, 162, 4, 4, HOLO_CORAL, 2, true, 0);
  s_wind_info = holo_label(s_metrics_group, "", &app_ui_font_14,
                           HOLO_APRICOT, 12, 201, 102, LV_TEXT_ALIGN_LEFT);
  s_wind_gust = holo_label(s_metrics_group, "", &app_ui_font_14,
                           HOLO_CREAM, 12, 218, 102, LV_TEXT_ALIGN_LEFT);
  holo_label(s_metrics_group, "湿度", &app_ui_font_14, HOLO_CREAM,
              130, 144, 96, LV_TEXT_ALIGN_CENTER);
  s_humidity_value = holo_label(s_metrics_group, "--", &lv_font_montserrat_32,
                                HOLO_MINT, 130, 171, 96, LV_TEXT_ALIGN_CENTER);
  s_feels_value = holo_label(s_metrics_group, "", &app_ui_font_14,
                             HOLO_CREAM, 130, 216, 96, LV_TEXT_ALIGN_CENTER);
  visible(s_metrics_group, false);
}
esp_err_t app_ui_init(void) {
  // Retained for the screen lifetime; never allocate 115200-byte images from
  // LVGL's pool.
  s_wallpaper_pixels = app_psram_zero(APP_MEDIA_IMAGE_BYTES);
  s_album_pixels = app_psram_zero(APP_MEDIA_IMAGE_BYTES);
  if (!s_wallpaper_pixels || !s_album_pixels)
    discard_unpublished_images(); // Preserve clock/voice/tools
                                  // when optional pictures fail.
  s_wallpaper_dsc.data = s_wallpaper_pixels;
  s_album_dsc.data = s_album_pixels;
  if (!lvgl_port_lock(0)) {
    discard_unpublished_images();
    return ESP_ERR_TIMEOUT;
  }
  for (unsigned i = 0; i < PAGE_COUNT; i++) {
    s_test_screens[i] = i == 0 ? lv_screen_active() : lv_obj_create(NULL);
    create_page(s_test_screens[i], i);
  }
  s_desktop_screen = lv_obj_create(NULL);
  app_desktop_create(s_desktop_screen);
  s_detail_screen = lv_obj_create(NULL);
  holo_surface(s_detail_screen);
  s_detail_title = holo_label(s_detail_screen, "", &app_ui_font_20,
                              HOLO_APRICOT, 20, 8, 200, LV_TEXT_ALIGN_CENTER);
  s_detail_body = holo_label(s_detail_screen, "", &app_ui_font_16, HOLO_CREAM,
                             20, 50, 200, LV_TEXT_ALIGN_LEFT);
  lv_label_set_long_mode(s_detail_body, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_height(s_detail_body, 144);
  lv_obj_set_style_text_line_space(s_detail_body, 0, 0);
  s_forecast_range = holo_label(s_detail_screen, "", &app_ui_font_14,
                                HOLO_APRICOT, 8, 43, 224, LV_TEXT_ALIGN_CENTER);
  s_forecast_group =
      holo_rect(s_detail_screen, 6, 66, 228, 122, HOLO_INK, 0, false, 0);
  lv_obj_add_flag(s_forecast_group, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(s_forecast_group, LV_OBJ_FLAG_SCROLL_ELASTIC |
      LV_OBJ_FLAG_SCROLL_MOMENTUM | LV_OBJ_FLAG_SCROLL_CHAIN_HOR | LV_OBJ_FLAG_SCROLL_CHAIN_VER);
  lv_obj_set_scroll_dir(s_forecast_group, LV_DIR_HOR);
  lv_obj_set_scrollbar_mode(s_forecast_group, LV_SCROLLBAR_MODE_OFF);
  s_forecast_strip = holo_rect(s_forecast_group, 0, 0, 228, 122, HOLO_INK, 0, false, 0);
  for (unsigned i = 0; i < FORECAST_SLOTS; ++i) {
    int x = (int)i * FORECAST_COLUMN_PX;
    s_forecast_time[i] = holo_label(s_forecast_strip, "", &app_ui_font_16,
                                    HOLO_CREAM, x, 0, 54, LV_TEXT_ALIGN_CENTER);
    for (unsigned j = 0; j < 10; ++j)
      s_forecast_shapes[i][j] =
          holo_rect(s_forecast_strip, x, 28, 1, 1, HOLO_CREAM, 0, true, 0);
    s_forecast_temp[i] =
        holo_label(s_forecast_strip, "", &app_ui_font_20, HOLO_CREAM, x, 73, 54,
                   LV_TEXT_ALIGN_CENTER);
    s_forecast_condition[i] =
        holo_label(s_forecast_strip, "", &app_ui_font_14, HOLO_APRICOT, x, 101,
                   54, LV_TEXT_ALIGN_CENTER);
  }
  s_forecast_status =
      holo_label(s_detail_screen, "", &app_ui_font_14, HOLO_APRICOT, 8, 190,
                 224, LV_TEXT_ALIGN_CENTER);
  s_detail_hint = holo_label(s_detail_screen, "", &app_ui_font_14, HOLO_MINT, 5,
                             216, 230, LV_TEXT_ALIGN_CENTER);
  for (unsigned i = 0; i < 12; ++i) {
    s_forecast_marks[i] = holo_rect(s_detail_screen, 24 + (int)i * 16, 220,
                                    12, 4, HOLO_MINT, 1, true, 0);
    visible(s_forecast_marks[i], false);
  }
  create_weather_metrics();
  visible(s_forecast_group, false);
  visible(s_forecast_range, false);
  visible(s_forecast_status, false);
  create_voice_overlay();
  s_album_screen = lv_obj_create(NULL);
  holo_surface(s_album_screen);
  s_album_image = lv_image_create(s_album_screen);
  if (s_album_dsc.data)
    lv_image_set_src(s_album_image, &s_album_dsc);
  s_album_info =
      holo_rect(s_album_screen, 0, 168, 240, 72, HOLO_INK, 0, true, 0);
  lv_obj_set_style_bg_opa(s_album_info, LV_OPA_80, 0);
  s_album_name = holo_label(s_album_info, "", &app_ui_font_16, HOLO_CREAM, 16,
                            4, 208, LV_TEXT_ALIGN_CENTER);
  s_album_status = holo_label(s_album_info, "", &app_ui_font_16, HOLO_APRICOT,
                              16, 25, 208, LV_TEXT_ALIGN_CENTER);
  s_album_hint = holo_label(s_album_info, "", &app_ui_font_16, HOLO_MINT, 16,
                            47, 208, LV_TEXT_ALIGN_CENTER);
  s_idle_screen = lv_obj_create(NULL);
  holo_surface(s_idle_screen);
  // A clock-only screensaver, independent of the home and legacy media flags.
  create_clock(s_idle_screen, &s_clocks[1]);
  s_last_activity = esp_timer_get_time();
  s_current_page_index = 0;
  lv_screen_load(s_test_screens[0]);
  lv_timer_t *mic_timer =
      lv_timer_create(update_mic_ui, MIC_UI_REFRESH_MS, NULL);
  lv_timer_t *clock_timer = lv_timer_create(update_home_time, 1000, NULL);
  lv_timer_t *detail_timer = lv_timer_create(detail_motion_tick, 33, NULL);
  lv_timer_t *wall_timer = lv_timer_create(wallpaper_tick, 100, NULL);
  update_home_time(NULL);
  lvgl_port_unlock();
  if (!mic_timer || !clock_timer || !detail_timer || !wall_timer)
    return ESP_ERR_NO_MEM;
  ESP_LOGI(TAG, "透空页面与状态层初始化完成");
  return ESP_OK;
}
