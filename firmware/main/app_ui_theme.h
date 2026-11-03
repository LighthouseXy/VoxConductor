#pragma once
#include "lvgl.h"
#include <stdbool.h>

// V3: sparse warm objects on unlit pixels, not a translucent LCD panel.
#define HOLO_INK 0x000000
#define HOLO_CREAM 0xFFF1C9
#define HOLO_APRICOT 0xFFC66E
#define HOLO_CORAL 0xFF9B7D
#define HOLO_MINT 0xAFE3C9
LV_FONT_DECLARE(app_ui_font_16);
LV_FONT_DECLARE(app_ui_font_20);
LV_FONT_DECLARE(app_ui_font_digits_56);

static inline void holo_surface(lv_obj_t *object) {
  lv_obj_remove_style_all(object);
  lv_obj_set_style_bg_color(object, lv_color_hex(HOLO_INK), 0);
  lv_obj_set_style_bg_opa(object, LV_OPA_COVER, 0);
  lv_obj_set_style_pad_all(object, 0, 0);
  lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
}
static inline lv_obj_t *holo_rect(lv_obj_t *parent, int x, int y, int w, int h,
                                  unsigned color, int radius, bool filled,
                                  unsigned stroke) {
  lv_obj_t *object = lv_obj_create(parent);
  holo_surface(object);
  lv_obj_set_pos(object, x, y);
  lv_obj_set_size(object, w, h);
  lv_obj_set_style_radius(object, radius, 0);
  lv_obj_set_style_bg_color(object, lv_color_hex(color), 0);
  lv_obj_set_style_bg_opa(object, filled ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_color(object, lv_color_hex(color), 0);
  lv_obj_set_style_border_width(object, stroke, 0);
  return object;
}
static inline lv_obj_t *holo_label(lv_obj_t *parent, const char *text,
                                   const lv_font_t *font, unsigned color, int x,
                                   int y, int w, lv_text_align_t align) {
  lv_obj_t *label = lv_label_create(parent);
  lv_obj_remove_style_all(label);
  lv_obj_set_pos(label, x, y);
  lv_obj_set_width(label, w);
  lv_obj_set_style_text_font(label, font, 0);
  lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
  lv_obj_set_style_text_align(label, align, 0);
  lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
  lv_label_set_text(label, text);
  return label;
}
typedef struct {
  lv_obj_t *group, *mouth, *eyes[2];
  unsigned unit;
} holo_robot_t;
static inline holo_robot_t holo_robot_create(lv_obj_t *parent, int x, int y,
                                             unsigned unit) {
  holo_robot_t robot = {.unit = unit};
  robot.group =
      holo_rect(parent, x, y, 48 * unit, 48 * unit, HOLO_INK, 0, false, 0);
  holo_rect(robot.group, 23 * unit, 1 * unit, 2 * unit, 7 * unit, HOLO_APRICOT,
            unit, true, 0);
  holo_rect(robot.group, 21 * unit, 0, 6 * unit, 6 * unit, HOLO_CREAM,
            LV_RADIUS_CIRCLE, true, 0);
  holo_rect(robot.group, 5 * unit, 9 * unit, 38 * unit, 23 * unit, HOLO_CORAL,
            8 * unit, false, 2 * unit);
  holo_rect(robot.group, 10 * unit, 13 * unit, 28 * unit, 15 * unit, HOLO_CREAM,
            4 * unit, false, 2 * unit);
  for (unsigned i = 0; i < 2; i++)
    robot.eyes[i] = holo_rect(robot.group, (16 + i * 11) * unit, 17 * unit,
                              4 * unit, 6 * unit, HOLO_MINT, 2 * unit, true, 0);
  robot.mouth = holo_rect(robot.group, 22 * unit, 24 * unit, 4 * unit, unit,
                          HOLO_APRICOT, unit, true, 0);
  holo_rect(robot.group, 14 * unit, 33 * unit, 20 * unit, 13 * unit, HOLO_CREAM,
            4 * unit, false, 2 * unit);
  holo_rect(robot.group, 21 * unit, 36 * unit, 6 * unit, 6 * unit, HOLO_APRICOT,
            unit, true, 0);
  holo_rect(robot.group, 7 * unit, 35 * unit, 5 * unit, 8 * unit, HOLO_CORAL,
            2 * unit, true, 0);
  holo_rect(robot.group, 36 * unit, 35 * unit, 5 * unit, 8 * unit, HOLO_CORAL,
            2 * unit, true, 0);
  return robot;
}
static inline void holo_robot_pose(holo_robot_t *robot, unsigned frame,
                                   bool sad) {
  if (!robot->mouth)
    return;
  unsigned unit = robot->unit;
  lv_obj_set_height(robot->mouth, (sad ? 1 : 1 + frame % 3) * unit);
  lv_obj_set_style_bg_color(robot->mouth,
                            lv_color_hex(sad ? HOLO_CORAL : HOLO_APRICOT), 0);
  for (unsigned i = 0; i < 2; i++)
    lv_obj_set_height(robot->eyes[i], (sad ? 2 : 6) * unit);
}
static inline lv_obj_t *holo_tool_icon(lv_obj_t *parent, unsigned tool, int x,
                                       int y) {
  lv_obj_t *group = holo_rect(parent, x, y, 80, 72, HOLO_INK, 0, false, 0);
  if (tool == 0) {
    holo_rect(group, 5, 18, 70, 43, HOLO_CORAL, 15, false, 3);
    holo_rect(group, 17, 36, 22, 5, HOLO_APRICOT, 2, true, 0);
    holo_rect(group, 25, 28, 5, 22, HOLO_APRICOT, 2, true, 0);
    holo_rect(group, 50, 29, 9, 9, HOLO_CREAM, LV_RADIUS_CIRCLE, true, 0);
    holo_rect(group, 59, 40, 9, 9, HOLO_APRICOT, LV_RADIUS_CIRCLE, true, 0);
  } else if (tool == 1) {
    holo_rect(group, 17, 12, 46, 46, HOLO_CREAM, LV_RADIUS_CIRCLE, false, 3);
    holo_rect(group, 37, 6, 8, 6, HOLO_APRICOT, 2, true, 0);
    holo_rect(group, 38, 23, 3, 16, HOLO_APRICOT, 1, true, 0);
    holo_rect(group, 40, 37, 14, 3, HOLO_CORAL, 1, true, 0);
  } else {
    holo_rect(group, 18, 9, 44, 54, HOLO_CREAM, 7, false, 3);
    for (unsigned i = 0; i < 3; i++) {
      holo_rect(group, 25, 21 + i * 12, 5, 5, HOLO_APRICOT, 1, false, 2);
      holo_rect(group, 35, 23 + i * 12, 18, 2, HOLO_CORAL, 1, true, 0);
    }
  }
  return group;
}
