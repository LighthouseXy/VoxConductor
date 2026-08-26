#include "app_ui.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

// 项目精简中文字体，覆盖固定界面文案，并回退到LVGL内置CJK字体。
LV_FONT_DECLARE(app_ui_font_14);
LV_FONT_DECLARE(app_ui_font_weather_24);

#define LCD_WIDTH 240
#define LCD_HEIGHT 240

#define MIC_UI_NOISE_FLOOR 12000
#define MIC_UI_FULL_SCALE 300000
#define MIC_UI_REFRESH_MS 100
#define VOICE_ANIMATION_REFRESH_MS 40
#define VOICE_STATE_SPEAKING_INDEX 2

typedef struct {
  const char *title;
  const char *subtitle;
  uint32_t background_color;
  uint32_t accent_color;
} test_page_desc_t;

static const test_page_desc_t TEST_PAGES[] = {
    {"时钟", "时间与天气", 0x081018, 0x38D996},
    {"天气", "等待数据", 0x101427, 0x3BA7FF},
    {"语音助手", "长按开始对话", 0x172033, 0x3BA7FF},
};

static const test_page_desc_t VOICE_STATES[] = {
    {"聆听中", "请开始说话", 0x10243A, 0x3BA7FF},
    {"思考中", "正在处理语音", 0x201A35, 0xA78BFA},
    {"回答中", "正在播放回答", 0x102A22, 0x4ADE80},
    {"出错了", "请稍后重试", 0x351418, 0xFF5C70},
};

#define TEST_PAGE_COUNT (sizeof(TEST_PAGES) / sizeof(TEST_PAGES[0]))
#define VOICE_STATE_COUNT (sizeof(VOICE_STATES) / sizeof(VOICE_STATES[0]))
#define VOICE_STATE_LISTENING_INDEX 0

#define UI_COLOR_BACKGROUND 0x000000
#define UI_COLOR_SURFACE 0x000000
#define UI_COLOR_PRIMARY 0x82E6C3
#define UI_COLOR_WEATHER 0x72D7C2
#define UI_COLOR_TEXT 0xF5F7F2
#define UI_COLOR_MUTED 0xA5B2AB
#define UI_COLOR_BORDER 0x2C3B35
#define UI_COLOR_OFFLINE 0xE78B75
#define UI_COLOR_DATE 0xB8FFE7

static const char *TAG = "app_ui";

static _Atomic uint32_t s_mic_level;
static _Atomic bool s_wifi_connected;

static int32_t s_displayed_mic_percent;
static lv_obj_t *s_time_label;
static lv_obj_t *s_date_label;
static lv_obj_t *s_home_network_icon;
static lv_obj_t *s_home_network_slash;
static lv_obj_t *s_voice_page_network_icon;
static lv_obj_t *s_voice_page_network_slash;
static lv_obj_t *s_home_weather_value;
static lv_obj_t *s_home_weather_status;
static lv_obj_t *s_weather_page_city;
static lv_obj_t *s_weather_page_temperature;
static lv_obj_t *s_weather_page_status;
static lv_obj_t *s_weather_page_apparent;
static lv_obj_t *s_weather_page_updated_at;
static uint16_t *s_ink_background_pixels;

static lv_obj_t *s_test_screens[TEST_PAGE_COUNT];
static size_t s_current_page_index;
static lv_obj_t *s_voice_overlay;
static lv_obj_t *s_voice_wave_inner;
static lv_obj_t *s_voice_wave_outer;
static lv_obj_t *s_voice_marker;
static lv_obj_t *s_voice_title;
static lv_obj_t *s_voice_subtitle;
static size_t s_active_voice_state = VOICE_STATE_COUNT;
static uint8_t s_voice_animation_phase;

static bool prepare_ink_background(void) {
  if (s_ink_background_pixels != NULL) {
    return true;
  }

  const size_t pixel_count = LCD_WIDTH * LCD_HEIGHT;
  const size_t buffer_size = pixel_count * sizeof(uint16_t);

  s_ink_background_pixels =
      heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

  if (s_ink_background_pixels == NULL) {
    ESP_LOGE(TAG, "无法为墨水背景分配PSRAM");
    return false;
  }

  const int32_t center_x = LCD_WIDTH / 2;
  // 渐变圆心向下移动，给顶部标题留出更多空间
  const int32_t center_y = LCD_HEIGHT / 2 + 10;
  // 缩小渐变区域，让屏幕外围保留更宽的纯黑区域
  const uint32_t gradient_radius = 105;
  const uint32_t radius_squared = gradient_radius * gradient_radius;

  for (int32_t y = 0; y < LCD_HEIGHT; y++) {
    for (int32_t x = 0; x < LCD_WIDTH; x++) {
      const int32_t dx = x - center_x;
      const int32_t dy = y - center_y;
      const uint32_t distance_squared = (uint32_t)(dx * dx + dy * dy);

      uint32_t strength = 0;

      if (distance_squared < radius_squared) {
        // 中心为255，接近四周时逐渐变为0
        strength =
            ((radius_squared - distance_squared) * 255U) / radius_squared;

        // 二次衰减让边缘更快融入纯黑
        strength = (strength * strength) / 255U;
      }

      // 中心保持很暗，避免棱镜中出现明显发光方块
      // 使用更高饱和度的青绿色，补偿棱镜造成的颜色损失
      uint8_t red = (20 * strength) / 255;
      uint8_t green = (130 * strength) / 255;
      uint8_t blue = (95 * strength) / 255;
      s_ink_background_pixels[y * LCD_WIDTH + x] =
          lv_color_to_u16(lv_color_make(red, green, blue));
    }
  }

  ESP_LOGI(TAG, "墨水背景生成完成：%u字节", (unsigned)buffer_size);
  return true;
}

static void create_ink_background(lv_obj_t *screen) {
  if (!prepare_ink_background()) {
    // 分配失败时仍可继续显示纯黑背景
    return;
  }

  lv_obj_t *background = lv_canvas_create(screen);

  // 多个页面可以引用同一个只读背景缓冲
  lv_canvas_set_buffer(background, s_ink_background_pixels, LCD_WIDTH,
                       LCD_HEIGHT, LV_COLOR_FORMAT_RGB565);

  lv_obj_clear_flag(background, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_clear_flag(background, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_align(background, LV_ALIGN_CENTER, 0, 0);
}

static void create_page_indicator(lv_obj_t *screen, size_t active_index) {
  for (size_t i = 0; i < TEST_PAGE_COUNT; i++) {
    const bool active = i == active_index;

    lv_obj_t *dot = lv_obj_create(screen);
    lv_obj_set_size(dot, active ? 14 : 6, 6);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_border_width(dot, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(
        dot, lv_color_hex(active ? UI_COLOR_PRIMARY : UI_COLOR_MUTED),
        LV_PART_MAIN);
    lv_obj_set_style_bg_opa(dot, active ? LV_OPA_COVER : LV_OPA_40,
                            LV_PART_MAIN);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);

    const int32_t x_offset =
        ((int32_t)i - ((int32_t)TEST_PAGE_COUNT - 1) / 2) * 20;

    lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, x_offset, -8);
  }
}

static void create_home_page_content(lv_obj_t *screen) {
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
  // 背景必须首先创建，后续文字和控件才能显示在它上面
  create_ink_background(screen);

  // 顶部品牌文字
  lv_obj_t *brand = lv_label_create(screen);
  lv_label_set_text(brand, "VOXCONDUCTOR");
  lv_obj_set_style_text_font(brand, &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(brand, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
  lv_obj_align(brand, LV_ALIGN_TOP_LEFT, 14, 12);

  // 使用透明容器叠放Wi-Fi图标与断网斜线
  lv_obj_t *wifi_container = lv_obj_create(screen);
  lv_obj_set_size(wifi_container, 28, 24);
  lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifi_container, 0, LV_PART_MAIN);
  lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(wifi_container, LV_ALIGN_TOP_RIGHT, -12, 7);

  // LVGL内置Wi-Fi图标：中心点向上扩散
  s_home_network_icon = lv_label_create(wifi_container);
  lv_label_set_text(s_home_network_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(s_home_network_icon,
                              lv_color_hex(UI_COLOR_OFFLINE), LV_PART_MAIN);
  lv_obj_center(s_home_network_icon);

  // 断网时覆盖在Wi-Fi图标上的斜线
  s_home_network_slash = lv_label_create(wifi_container);
  lv_label_set_text(s_home_network_slash, "\\");
  lv_obj_set_style_text_font(s_home_network_slash, &lv_font_montserrat_20,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_home_network_slash,
                              lv_color_hex(UI_COLOR_OFFLINE), LV_PART_MAIN);
  lv_obj_center(s_home_network_slash);
  // 主时间使用48号字体，成为页面视觉中心
  s_time_label = lv_label_create(screen);
  lv_label_set_text(s_time_label, "--:--");
  lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_48,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_time_label, lv_color_hex(UI_COLOR_TEXT),
                              LV_PART_MAIN);
  lv_obj_align(s_time_label, LV_ALIGN_CENTER, 0, -24);

  s_date_label = lv_label_create(screen);
  lv_label_set_text(s_date_label, "等待校时");
  lv_obj_set_style_text_font(s_date_label,
                             &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_date_label, lv_color_hex(UI_COLOR_DATE),
                              LV_PART_MAIN);
  lv_obj_align(s_date_label, LV_ALIGN_CENTER, 0, 24);

  // 首页只保留一行天气摘要，避免重复显示“天气”标题。
  lv_obj_t *weather_card = lv_obj_create(screen);
  lv_obj_set_size(weather_card, 180, 30);
  lv_obj_align(weather_card, LV_ALIGN_BOTTOM_MID, 0, -31);
  lv_obj_set_style_border_width(weather_card, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(weather_card, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_pad_all(weather_card, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_column(weather_card, 12, LV_PART_MAIN);
  lv_obj_set_flex_flow(weather_card, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(weather_card, LV_FLEX_ALIGN_CENTER,
                        LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_clear_flag(weather_card, LV_OBJ_FLAG_SCROLLABLE);

  s_home_weather_value = lv_label_create(weather_card);
  lv_label_set_text(s_home_weather_value, "-- °C");
  lv_obj_set_style_text_font(s_home_weather_value, &lv_font_montserrat_20,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_home_weather_value,
                              lv_color_hex(UI_COLOR_WEATHER), LV_PART_MAIN);

  s_home_weather_status = lv_label_create(weather_card);
  lv_label_set_text(s_home_weather_status, "暂无数据");
  lv_obj_set_style_text_font(s_home_weather_status,
                             &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_home_weather_status,
                              lv_color_hex(UI_COLOR_MUTED), LV_PART_MAIN);

  create_page_indicator(screen, 0);
}

static void create_weather_page_content(lv_obj_t *screen) {
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  // 与首页共用中心渐变背景
  create_ink_background(screen);

  // 顶部显示地点和数据更新时间，不再重复显示“天气”标题。
  s_weather_page_city = lv_label_create(screen);
  lv_label_set_text(s_weather_page_city, "--");
  lv_obj_set_style_text_font(s_weather_page_city, &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_weather_page_city,
                              lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
  lv_obj_align(s_weather_page_city, LV_ALIGN_TOP_LEFT, 14, 12);

  s_weather_page_updated_at = lv_label_create(screen);
  lv_label_set_text(s_weather_page_updated_at, "等待更新");
  lv_obj_set_style_text_font(s_weather_page_updated_at, &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_weather_page_updated_at,
                              lv_color_hex(UI_COLOR_MUTED), LV_PART_MAIN);
  lv_obj_align(s_weather_page_updated_at, LV_ALIGN_TOP_RIGHT, -14, 12);

  // 天气状态位于视觉中心，并使用状态色强调。
  s_weather_page_status = lv_label_create(screen);
  lv_label_set_text(s_weather_page_status, "暂无数据");
  lv_obj_set_style_text_font(s_weather_page_status, &app_ui_font_weather_24,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_weather_page_status,
                              lv_color_hex(UI_COLOR_WEATHER), LV_PART_MAIN);
  lv_obj_align(s_weather_page_status, LV_ALIGN_CENTER, 0, 24);

  // 温度是天气页的主视觉信息。
  s_weather_page_temperature = lv_label_create(screen);
  lv_label_set_text(s_weather_page_temperature, "--°");
  lv_obj_set_style_text_font(s_weather_page_temperature,
                             &lv_font_montserrat_48, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_weather_page_temperature,
                              lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
  lv_obj_align(s_weather_page_temperature, LV_ALIGN_CENTER, 0, -34);

  s_weather_page_apparent = lv_label_create(screen);
  lv_label_set_text(s_weather_page_apparent, "体感 --°");
  lv_obj_set_style_text_font(s_weather_page_apparent, &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_weather_page_apparent,
                              lv_color_hex(UI_COLOR_DATE), LV_PART_MAIN);
  lv_obj_align(s_weather_page_apparent, LV_ALIGN_CENTER, 0, 58);

  create_page_indicator(screen, 1);
}

static void create_voice_page_content(lv_obj_t *screen) {
  lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_set_style_bg_color(screen, lv_color_hex(UI_COLOR_BACKGROUND),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);

  // 与其他基础页面共用渐变背景
  create_ink_background(screen);

  lv_obj_t *title = lv_label_create(screen);
  lv_label_set_text(title, "语音助手");
  lv_obj_set_style_text_font(title, &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(title, lv_color_hex(UI_COLOR_TEXT), LV_PART_MAIN);
  lv_obj_align(title, LV_ALIGN_TOP_LEFT, 14, 12);

  lv_obj_t *wifi_container = lv_obj_create(screen);
  lv_obj_set_size(wifi_container, 28, 24);
  lv_obj_set_style_bg_opa(wifi_container, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(wifi_container, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(wifi_container, 0, LV_PART_MAIN);
  lv_obj_clear_flag(wifi_container, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(wifi_container, LV_ALIGN_TOP_RIGHT, -12, 7);

  s_voice_page_network_icon = lv_label_create(wifi_container);
  lv_label_set_text(s_voice_page_network_icon, LV_SYMBOL_WIFI);
  lv_obj_set_style_text_color(s_voice_page_network_icon,
                              lv_color_hex(UI_COLOR_MUTED), LV_PART_MAIN);
  lv_obj_center(s_voice_page_network_icon);

  s_voice_page_network_slash = lv_label_create(wifi_container);
  lv_label_set_text(s_voice_page_network_slash, "\\");
  lv_obj_set_style_text_font(s_voice_page_network_slash,
                             &lv_font_montserrat_20, LV_PART_MAIN);
  lv_obj_set_style_text_color(s_voice_page_network_slash,
                              lv_color_hex(UI_COLOR_OFFLINE), LV_PART_MAIN);
  lv_obj_center(s_voice_page_network_slash);

  lv_obj_t *voice_ring = lv_obj_create(screen);
  lv_obj_set_size(voice_ring, 76, 76);
  lv_obj_set_style_radius(voice_ring, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(voice_ring, lv_color_hex(UI_COLOR_PRIMARY),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(voice_ring, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_border_width(voice_ring, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(voice_ring, lv_color_hex(UI_COLOR_PRIMARY),
                                LV_PART_MAIN);
  lv_obj_set_style_border_opa(voice_ring, LV_OPA_80, LV_PART_MAIN);
  lv_obj_clear_flag(voice_ring, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(voice_ring, LV_ALIGN_CENTER, 0, -34);

  lv_obj_t *voice_title = lv_label_create(screen);
  lv_label_set_text(voice_title, "长按开始对话");
  lv_obj_set_style_text_font(voice_title, &app_ui_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(voice_title, lv_color_hex(UI_COLOR_PRIMARY),
                              LV_PART_MAIN);
  lv_obj_align(voice_title, LV_ALIGN_CENTER, 0, 26);

  lv_obj_t *voice_hint = lv_label_create(screen);
  lv_label_set_text(voice_hint, "按住说话，松开发送");
  lv_obj_set_style_text_font(voice_hint, &app_ui_font_14, LV_PART_MAIN);
  lv_obj_set_style_text_color(voice_hint, lv_color_hex(UI_COLOR_MUTED),
                              LV_PART_MAIN);
  lv_obj_align(voice_hint, LV_ALIGN_CENTER, 0, 53);

  create_page_indicator(screen, 2);
}

static void create_test_page_content(lv_obj_t *screen, size_t index)
{
    if (index == 0) {
        create_home_page_content(screen);
        return;
    }

    if (index == 1) {
        create_weather_page_content(screen);
        return;
    }

    if (index == 2) {
        create_voice_page_content(screen);
        return;
    }

    // 防止将来页面数量修改后遗漏对应的创建逻辑
    ESP_LOGE(TAG, "不存在的页面索引：%u", (unsigned)index);
}

static void show_page(size_t page_index, lv_screen_load_anim_t animation) {
  if (page_index >= TEST_PAGE_COUNT || page_index == s_current_page_index) {
    return;
  }

  s_current_page_index = page_index;

  // false 表示页面切走后不销毁，因为这些页面需要反复使用
  lv_screen_load_anim(s_test_screens[s_current_page_index], animation, 220, 0,
                      false);

  ESP_LOGI(TAG, "切换基础页面：%s", TEST_PAGES[s_current_page_index].title);
}

void app_ui_show_next_page(void) {
  const size_t next_page = (s_current_page_index + 1) % TEST_PAGE_COUNT;

  show_page(next_page, LV_SCREEN_LOAD_ANIM_MOVE_LEFT);
}

void app_ui_show_previous_page(void) {
  const size_t previous_page =
      (s_current_page_index + TEST_PAGE_COUNT - 1) % TEST_PAGE_COUNT;

  show_page(previous_page, LV_SCREEN_LOAD_ANIM_MOVE_RIGHT);
}

static void create_voice_overlay(void) {
  // 覆盖层始终位于基础页面上方
  s_voice_overlay = lv_obj_create(lv_layer_top());
  lv_obj_set_size(s_voice_overlay, LCD_WIDTH, LCD_HEIGHT);
  lv_obj_set_style_radius(s_voice_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_voice_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_pad_all(s_voice_overlay, 0, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_voice_overlay, lv_color_hex(UI_COLOR_BACKGROUND),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_voice_overlay, LV_OPA_COVER, LV_PART_MAIN);
  lv_obj_clear_flag(s_voice_overlay, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_center(s_voice_overlay);

  // 与基础页面使用相同渐变，不再显示整屏状态色
  create_ink_background(s_voice_overlay);

  // 两道扩散声波只用于“回答中”动画，不读取麦克风音量。
  s_voice_wave_outer = lv_obj_create(s_voice_overlay);
  lv_obj_set_size(s_voice_wave_outer, 68, 68);
  lv_obj_set_style_radius(s_voice_wave_outer, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_voice_wave_outer, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_voice_wave_outer, 2, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_voice_wave_outer, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_clear_flag(s_voice_wave_outer, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_voice_wave_outer, LV_ALIGN_CENTER, 0, -55);
  lv_obj_set_style_transform_pivot_x(s_voice_wave_outer, 34, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(s_voice_wave_outer, 34, LV_PART_MAIN);

  s_voice_wave_inner = lv_obj_create(s_voice_overlay);
  lv_obj_set_size(s_voice_wave_inner, 68, 68);
  lv_obj_set_style_radius(s_voice_wave_inner, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_voice_wave_inner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_voice_wave_inner, 2, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_voice_wave_inner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_clear_flag(s_voice_wave_inner, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_voice_wave_inner, LV_ALIGN_CENTER, 0, -55);
  lv_obj_set_style_transform_pivot_x(s_voice_wave_inner, 34, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(s_voice_wave_inner, 34, LV_PART_MAIN);

  // 状态圆环只用于表达当前语音阶段，不使用实心色块
  s_voice_marker = lv_obj_create(s_voice_overlay);
  lv_obj_set_size(s_voice_marker, 68, 68);
  lv_obj_set_style_radius(s_voice_marker, LV_RADIUS_CIRCLE, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_voice_marker, lv_color_hex(UI_COLOR_PRIMARY),
                            LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_voice_marker, LV_OPA_20, LV_PART_MAIN);
  lv_obj_set_style_border_width(s_voice_marker, 2, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_voice_marker, lv_color_hex(UI_COLOR_PRIMARY),
                                LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_voice_marker, LV_OPA_80, LV_PART_MAIN);
  lv_obj_clear_flag(s_voice_marker, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_align(s_voice_marker, LV_ALIGN_CENTER, 0, -55);

  // 语音圆环缩放时保持中心不动
  lv_obj_set_style_transform_pivot_x(s_voice_marker, 34, LV_PART_MAIN);
  lv_obj_set_style_transform_pivot_y(s_voice_marker, 34, LV_PART_MAIN);

  // 状态名称放在圆环下方，不再塞进圆形内部
  s_voice_title = lv_label_create(s_voice_overlay);
  lv_label_set_text(s_voice_title, "聆听中");
  lv_obj_set_style_text_font(s_voice_title,
                             &app_ui_font_14,
                             LV_PART_MAIN);
  lv_obj_set_style_text_color(s_voice_title, lv_color_hex(UI_COLOR_PRIMARY),
                              LV_PART_MAIN);
  lv_obj_align(s_voice_title, LV_ALIGN_CENTER, 0, 8);

  // 状态说明或AI回答
  s_voice_subtitle = lv_label_create(s_voice_overlay);
  lv_label_set_text(s_voice_subtitle, "请开始说话");
  lv_obj_set_style_text_color(s_voice_subtitle, lv_color_hex(UI_COLOR_TEXT),
                              LV_PART_MAIN);
  lv_obj_set_style_text_font(s_voice_subtitle,
                             &app_ui_font_14, LV_PART_MAIN);
  lv_label_set_long_mode(s_voice_subtitle, LV_LABEL_LONG_MODE_WRAP);
  lv_obj_set_width(s_voice_subtitle, 204);
  lv_obj_set_style_text_align(s_voice_subtitle, LV_TEXT_ALIGN_CENTER,
                              LV_PART_MAIN);
  lv_obj_align(s_voice_subtitle, LV_ALIGN_CENTER, 0, 52);

  // 默认不显示，开始语音流程时再解除隐藏
  lv_obj_add_flag(s_voice_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void show_voice_state(size_t index) {
  if (index >= VOICE_STATE_COUNT) {
    return;
  }

  const test_page_desc_t *state = &VOICE_STATES[index];
  const lv_color_t accent = lv_color_hex(state->accent_color);

  s_active_voice_state = index;

  s_voice_animation_phase = 0;

  lv_obj_set_style_transform_scale(s_voice_wave_inner, 256, LV_PART_MAIN);
  lv_obj_set_style_transform_scale(s_voice_wave_outer, 256, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_voice_wave_inner, LV_OPA_TRANSP, LV_PART_MAIN);
  lv_obj_set_style_border_opa(s_voice_wave_outer, LV_OPA_TRANSP, LV_PART_MAIN);
  // 切换状态时先恢复圆环原始大小
  lv_obj_set_style_transform_scale(s_voice_marker, 256, LV_PART_MAIN);
  lv_obj_set_style_bg_opa(s_voice_marker, LV_OPA_20, LV_PART_MAIN);

  // 状态只通过圆环、微弱内部光晕和标题颜色表达
  lv_obj_set_style_border_color(s_voice_marker, accent, LV_PART_MAIN);
  lv_obj_set_style_bg_color(s_voice_marker, accent, LV_PART_MAIN);

  lv_obj_set_style_border_color(s_voice_wave_inner, accent, LV_PART_MAIN);
  lv_obj_set_style_border_color(s_voice_wave_outer, accent, LV_PART_MAIN);

  lv_obj_set_style_text_color(s_voice_title, accent, LV_PART_MAIN);

  lv_obj_set_style_text_color(s_voice_subtitle, lv_color_hex(UI_COLOR_TEXT),
                              LV_PART_MAIN);

  lv_label_set_text(s_voice_title, state->title);
  lv_label_set_text(s_voice_subtitle, state->subtitle);

  lv_obj_remove_flag(s_voice_overlay, LV_OBJ_FLAG_HIDDEN);

  ESP_LOGI(TAG, "显示语音状态: %s", state->title);
}

static void show_chat_overlay(size_t state_index, const char *message) {
  // 网络请求运行在main任务中，操作LVGL前必须加锁
  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "获取LVGL锁失败");
    return;
  }

  show_voice_state(state_index);

  if (message != NULL) {
    lv_label_set_text(s_voice_subtitle, message);
  }

  lvgl_port_unlock();
}

static void hide_chat_overlay(void) {
  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "获取LVGL锁失败");
    return;
  }

  // 隐藏顶层语音界面，回到开始语音前所在的基础页面。
  lv_obj_add_flag(s_voice_overlay, LV_OBJ_FLAG_HIDDEN);

  s_active_voice_state = VOICE_STATE_COUNT;
  lvgl_port_unlock();

  ESP_LOGI(TAG, "AI回答显示结束，返回之前页面");
}

static void update_voice_animation(lv_timer_t *timer) {
  (void)timer;

  if (s_active_voice_state != VOICE_STATE_SPEAKING_INDEX) {
    return;
  }

  // 中心圆使用三角波轻微呼吸。
  const uint8_t triangle =
      s_voice_animation_phase < 128
          ? s_voice_animation_phase
          : (uint8_t)(255 - s_voice_animation_phase);

  const int32_t marker_scale =
      256 + ((int32_t)triangle * 32) / 127;

  lv_obj_set_style_transform_scale(
      s_voice_marker, marker_scale, LV_PART_MAIN);

  // 两道声波错开半个周期，向外扩散并淡出。
  const uint8_t inner_phase = s_voice_animation_phase;
  const uint8_t outer_phase =
      (uint8_t)(s_voice_animation_phase + 128);

  lv_obj_set_style_transform_scale(
      s_voice_wave_inner,
      256 + ((int32_t)inner_phase * 112) / 255,
      LV_PART_MAIN);

  lv_obj_set_style_transform_scale(
      s_voice_wave_outer,
      256 + ((int32_t)outer_phase * 112) / 255,
      LV_PART_MAIN);

  lv_obj_set_style_border_opa(
      s_voice_wave_inner,
      (lv_opa_t)(((255 - inner_phase) * LV_OPA_50) / 255),
      LV_PART_MAIN);

  lv_obj_set_style_border_opa(
      s_voice_wave_outer,
      (lv_opa_t)(((255 - outer_phase) * LV_OPA_50) / 255),
      LV_PART_MAIN);

  s_voice_animation_phase += 8;
}

static void update_mic_ui(lv_timer_t *timer)
{
  (void)timer;

  const uint32_t level =
      atomic_load_explicit(
          &s_mic_level,
          memory_order_relaxed);

  int32_t percent = 0;

  if (level >= MIC_UI_FULL_SCALE) {
    percent = 100;
  } else if (level > MIC_UI_NOISE_FLOOR) {
    percent =
        (int32_t)(
            ((uint64_t)(level - MIC_UI_NOISE_FLOOR) * 100) /
            (MIC_UI_FULL_SCALE - MIC_UI_NOISE_FLOOR));
  }

  // 对显示值再次低通，避免圆环忽大忽小地闪烁
  s_displayed_mic_percent =
      (s_displayed_mic_percent * 3 + percent) / 4;

  // LVGL缩放值256代表原始大小
  const int32_t ring_scale =
      256 + (s_displayed_mic_percent * 104) / 100;

  const lv_opa_t ring_opa =
      (lv_opa_t)(
          LV_OPA_20 +
          (s_displayed_mic_percent *
           (LV_OPA_60 - LV_OPA_20)) /
              100);

  // 只有LISTENING状态才让语音圆环随声音变化
  if (s_voice_marker != NULL &&
      s_active_voice_state ==
          VOICE_STATE_LISTENING_INDEX) {
    lv_obj_set_style_transform_scale(
        s_voice_marker,
        ring_scale,
        LV_PART_MAIN);

    lv_obj_set_style_bg_opa(
        s_voice_marker,
        ring_opa,
        LV_PART_MAIN);
  }
}

static void update_home_time(lv_timer_t *timer) {
  (void)timer;

  if (s_time_label == NULL) {
    return;
  }

  // 联网状态与时间同步状态无关，因此每秒都更新
  const bool connected =
      atomic_load_explicit(&s_wifi_connected, memory_order_acquire);

  if (s_home_network_icon != NULL && s_home_network_slash != NULL) {
    // 已连接显示高亮Wi-Fi图标，断开时降低图标亮度
    lv_obj_set_style_text_color(
        s_home_network_icon,
        lv_color_hex(connected ? UI_COLOR_PRIMARY : UI_COLOR_MUTED),
        LV_PART_MAIN);

    if (connected) {
      // Wi-Fi连接成功后隐藏斜线
      lv_obj_add_flag(s_home_network_slash, LV_OBJ_FLAG_HIDDEN);
    } else {
      // 未连接时显示橙红色斜线
      lv_obj_clear_flag(s_home_network_slash, LV_OBJ_FLAG_HIDDEN);
    }
  }

  if (s_voice_page_network_icon != NULL &&
      s_voice_page_network_slash != NULL) {
    lv_obj_set_style_text_color(
        s_voice_page_network_icon,
        lv_color_hex(connected ? UI_COLOR_PRIMARY : UI_COLOR_MUTED),
        LV_PART_MAIN);

    if (connected) {
      lv_obj_add_flag(s_voice_page_network_slash, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_clear_flag(s_voice_page_network_slash, LV_OBJ_FLAG_HIDDEN);
    }
  }

  time_t now;
  struct tm local_time;

  time(&now);

  if (localtime_r(&now, &local_time) == NULL ||
      local_time.tm_year + 1900 < 2024) {
    // SNTP尚未完成时不显示1970年的错误时间
    lv_label_set_text(s_time_label, "--:--");

    if (s_date_label != NULL) {
      lv_label_set_text(s_date_label, "等待校时");
    }

    return;
  }

  char time_text[6];
  char date_text[24];

  if (strftime(time_text, sizeof(time_text), "%H:%M", &local_time) > 0) {
    lv_label_set_text(s_time_label, time_text);
  }

  if (s_date_label != NULL) {
    static const char *weekdays[] = {"周日", "周一", "周二", "周三",
                                     "周四", "周五", "周六"};

    snprintf(date_text, sizeof(date_text), "%02d月%02d日  %s",
             local_time.tm_mon + 1, local_time.tm_mday,
             weekdays[local_time.tm_wday]);

    lv_label_set_text(s_date_label, date_text);
  }
}

esp_err_t app_ui_init(void) {
  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "初始化UI时获取LVGL锁失败");
    return ESP_ERR_TIMEOUT;
  }

  for (size_t i = 0; i < TEST_PAGE_COUNT; i++) {
    s_test_screens[i] = (i == 0) ? lv_screen_active() : lv_obj_create(NULL);
    create_test_page_content(s_test_screens[i], i);
  }

  create_voice_overlay();

  s_current_page_index = 0;
  lv_screen_load(s_test_screens[s_current_page_index]);

  lv_timer_t *mic_timer =
      lv_timer_create(update_mic_ui, MIC_UI_REFRESH_MS, NULL);
  lv_timer_t *clock_timer = lv_timer_create(update_home_time, 1000, NULL);
  lv_timer_t *voice_animation_timer =
      lv_timer_create(update_voice_animation, VOICE_ANIMATION_REFRESH_MS, NULL);

  lvgl_port_unlock();

  if (mic_timer == NULL || clock_timer == NULL ||
      voice_animation_timer == NULL) {
    ESP_LOGE(TAG, "创建UI定时器失败");
    return ESP_ERR_NO_MEM;
  }
  ESP_LOGI(TAG, "UI页面与状态覆盖层初始化完成");
  return ESP_OK;
}

void app_ui_show_voice_state(app_ui_voice_state_t state, const char *message) {
  if ((size_t)state >= VOICE_STATE_COUNT) {
    ESP_LOGE(TAG, "无效语音状态：%d", (int)state);
    return;
  }

  show_chat_overlay((size_t)state, message);
}

void app_ui_hide_voice_state(void) {
  hide_chat_overlay();
}

void app_ui_set_mic_level(uint32_t level) {
  atomic_store_explicit(&s_mic_level, level, memory_order_release);
}

void app_ui_set_wifi_connected(bool connected) {
  atomic_store_explicit(&s_wifi_connected, connected, memory_order_release);
}


static uint32_t weather_accent_color(int weather_code)
{
  if (weather_code == 0) {
    return 0xFFD166;
  }
  if (weather_code >= 1 && weather_code <= 3) {
    return 0x9ED8E8;
  }
  if (weather_code == 45 || weather_code == 48) {
    return 0xB7C7C9;
  }
  if ((weather_code >= 71 && weather_code <= 77) ||
      weather_code == 85 || weather_code == 86) {
    return 0xD9F3FF;
  }
  if (weather_code >= 95) {
    return 0xC7A6FF;
  }
  if (weather_code >= 51 && weather_code <= 82) {
    return 0x64B5FF;
  }
  return UI_COLOR_WEATHER;
}

void app_ui_set_weather(const char *city, int temperature_c,
                        int apparent_c, const char *condition,
                        int weather_code, const char *updated_at)
{
  if (city == NULL || condition == NULL || updated_at == NULL) {
    return;
  }

  // 网络任务不能直接操作LVGL，必须先获取界面锁。
  if (!lvgl_port_lock(0)) {
    ESP_LOGE(TAG, "更新天气时获取LVGL锁失败");
    return;
  }

  if (s_home_weather_value != NULL) {
    lv_label_set_text_fmt(s_home_weather_value, "%d °C", temperature_c);
  }

  if (s_home_weather_status != NULL) {
    lv_label_set_text(s_home_weather_status, condition);
    lv_obj_set_style_text_color(
        s_home_weather_status,
        lv_color_hex(weather_accent_color(weather_code)), LV_PART_MAIN);
  }

  if (s_weather_page_city != NULL) {
    lv_label_set_text(s_weather_page_city, city[0] != '\0' ? city : "当前地点");
  }

  if (s_weather_page_temperature != NULL) {
    lv_label_set_text_fmt(s_weather_page_temperature, "%d°", temperature_c);
  }

  if (s_weather_page_status != NULL) {
    lv_label_set_text(s_weather_page_status, condition);
    lv_obj_set_style_text_color(
        s_weather_page_status,
        lv_color_hex(weather_accent_color(weather_code)), LV_PART_MAIN);
  }

  if (s_weather_page_apparent != NULL) {
    lv_label_set_text_fmt(s_weather_page_apparent,
                          "体感 %d°", apparent_c);
  }

  if (s_weather_page_updated_at != NULL) {
    const char *time_separator = strchr(updated_at, 'T');
    if (time_separator != NULL && strlen(time_separator + 1) >= 5) {
      lv_label_set_text_fmt(s_weather_page_updated_at,
                            "%.5s 更新", time_separator + 1);
    } else {
      lv_label_set_text(s_weather_page_updated_at, "刚刚更新");
    }
  }

  lvgl_port_unlock();
}
