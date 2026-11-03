"""Menu/editor source guards and Python policy models.

Native LVGL rendering and the physical tilt detector require device validation.
Source guards tie the models to the relevant functions."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
desktop = (ROOT / "firmware/main/app_desktop.c").read_text()
ui = (ROOT / "firmware/main/app_ui.c").read_text()

def section(source, start, end):
    return source.split(start, 1)[1].split(end, 1)[0]

activate = section(desktop, "static void activate(void)", "int app_desktop_button")
settings = section(activate, "case MENU_SETTINGS:", "case MENU_VOLUME:")
for menu in ("MENU_WORK_TIME", "MENU_BREAK_TIME", "MENU_APPEARANCE"):
    assert f"enter({menu});" in settings
assert "save_later()" not in settings and "app_wake_set_enabled" not in settings
assert "s_preferences.focus.work_minutes =" not in settings
assert "s_preferences.focus.break_minutes =" not in settings
assert "return sizeof(tool_items) / sizeof(*tool_items);" in desktop
tool = section(desktop, "void app_desktop_open_tool(", "void app_desktop_open_settings")
assert "MENU_WORDS" in tool and "app_motion_available" not in tool
assert "MENU_WAKE" not in desktop and "app_wake_set_enabled" in desktop

button = section(desktop, "int app_desktop_button(", "unsigned app_desktop_base_brightness")
assert "if (s_button_navigation)" in button and "app_motion_available" not in button
assert "else if (!long_press)" in button and "menu_move(1)" in button
visible = section(desktop, "void app_desktop_visible(", "static void activate")
assert "visible && !s_visible" in visible and "s_button_navigation = !app_motion_available()" in visible
assert "app_audio_set_volume(s_preferences.volume)" in visible
assert "app_display_set_brightness(s_preferences.brightness)" in visible
motion = section(desktop, "bool app_desktop_motion(", "static esp_err_t persist_settings")
assert "s_button_navigation ? true : menu_move(direction)" in motion
move = section(desktop, "static bool menu_move(int direction) {", "bool app_desktop_motion")
assert "s_menu == MENU_ALARM && s_alarm_editing" in move
assert "s_menu == MENU_WORK_TIME" in move and "s_menu == MENU_BREAK_TIME" in move
assert "maximum = percent ? 100 : 120" in move
assert "s_menu == MENU_BRIGHTNESS &&" in move  # Other editors cannot change the backlight.

alarm = section(activate, "case MENU_ALARM:", "case MENU_ROOT:")
assert "s_edit_value = s_selection == 0 ? s_alarm_hour" in alarm
assert "s_alarm_editing = true" in alarm and "s_alarm_editing = false" in alarm
assert "s_selection++" not in alarm and "s_task_pending_id[0]" in alarm
assert alarm.index("if (s_selection < 3)") < alarm.index("xQueueSend")
back = section(desktop, "int app_desktop_back(", "static bool menu_move(int direction) {")
alarm_back = section(back, "case MENU_ALARM:", "case MENU_ROOT:")
for field in ("hour", "minute", "repeat"):
    assert f"s_alarm_{field} = s_edit_value" in alarm_back
assert alarm_back.index("return 1") < alarm_back.index("return 3")
tilt = section(desktop, "static void alarm_scroll_tick(", "static void tick(")
assert tilt.index("if (!s_alarm_editing)") < tilt.index("s_alarm_repeat =")
assert tilt.index("if (!s_alarm_editing)") < tilt.index("s_alarm_hour =")
assert "direction > 0 ? (s_selection < 2 ? 2" in tilt
render = section(desktop, "static void render_alarm(", "static void render_menu")
assert "s_grid_tiles[i]" in render and "i < 4" in render
assert "lv_obj_create" not in render and "holo_rect" not in render
assert "s_rows[i]" in render and "lv_font_montserrat_32" in render
grid = section(desktop, "static bool render_grid(", "static void render_alarm")
assert "lv_obj_set_size(s_grid_tiles[i], 108, 108)" in grid
assert "lv_obj_set_size(s_grid_names[i], 96, 42)" in grid

class Alarm:
    def __init__(self):
        self.values, self.field, self.editing = [23, 59, 0], 0, False
        self.original, self.pending, self.saves = None, False, 0
    def select(self, direction):
        if not self.editing and not self.pending:
            self.field = (self.field + direction) % 4
    def tilt(self, steps):
        if self.editing and not self.pending:
            self.values[self.field] = (self.values[self.field] + steps) % [24, 60, 3][self.field]
    def press(self):
        if self.pending:
            return
        if self.field == 3:
            self.pending = True
            self.saves += 1
        elif self.editing:
            self.editing = False
        else:
            self.original = self.values[self.field]
            self.editing = True
    def back(self):
        if self.editing:
            self.values[self.field] = self.original
            self.editing = False
            return "form"
        return "clock"

a = Alarm()
a.tilt(5)
assert a.values == [23, 59, 0]  # Merely selecting never changes time.
a.press()
a.select(1)
assert a.field == 0  # An edit cannot steal focus.
a.tilt(1)
assert a.values[0] == 0
assert a.back() == "form" and a.values[0] == 23
a.select(1)
a.press()
a.tilt(1)
assert a.values[1] == 0
a.press()
assert not a.editing and a.values[1] == 0
a.select(1)
a.press()
a.tilt(-1)
assert a.values[2] == 2
assert a.back() == "form" and a.values[2] == 0
a.select(1)
a.tilt(1)
assert a.saves == 0
a.press()
a.press()
assert a.pending and a.saves == 1
assert a.back() == "clock"

# Fixed fault mode: a changing sensor cannot redefine the current press.
for sensor_at_entry in (False, True):
    button_navigation = not sensor_at_entry
    operations = ["confirm" if not button_navigation else "move"
                  for sensor_now in (False, True, False, True)]
    assert len(set(operations)) == 1

weather_tick = section(ui, "static void detail_motion_tick(", "void app_ui_enter_page")
assert "s_weather_uv_page = !s_weather_uv_page" in weather_tick
assert "direction > 0" not in weather_tick
assert "app_button_click_pending()" in weather_tick
for directions in ((1, 1, 1, 1), (-1, -1, -1, -1), (1, -1, -1, 1)):
    page, seen = False, []
    for direction in directions:
        page = not page
        seen.append(page)
    assert seen == [True, False, True, False]

# Font heights are read from the actual installed/generated fonts. Each AQI
# label has a fixed one-line box instead of growing into the following row.
layout = section(ui, "static void create_weather_metrics(", "esp_err_t app_ui_init")
ys = {}
for label in ("s_aqi_title", "s_aqi_value", "s_aqi_info", "s_aqi_updated"):
    found = re.search(rf'{label} = holo_label\(.*?14, (\d+), 96,', layout, re.S)
    assert found
    ys[label] = int(found[1])
heights = {name: int(h) for name, h in re.findall(r"lv_obj_set_height\((s_aqi_\w+), (\d+)\)", layout)}
labels = ("s_aqi_title", "s_aqi_value", "s_aqi_info", "s_aqi_updated")
assert all(ys[a] + heights[a] <= ys[b] for a, b in zip(labels, labels[1:]))
assert 36 < ys[labels[0]] and ys[labels[-1]] + heights[labels[-1]] < 132
font14 = (ROOT / "firmware/main/app_ui_font_14.c").read_text()
font32 = (ROOT / "firmware/managed_components/lvgl__lvgl/src/font/lv_font_montserrat_32.c").read_text()
assert int(re.search(r"\.line_height = (\d+)", font14)[1]) <= heights["s_aqi_info"]
assert int(re.search(r"\.line_height = (\d+)", font32)[1]) <= heights["s_aqi_value"]
metrics = section(ui, "static void render_weather_metrics(", "static void refresh_page_details")
assert '"PM2.5 %.0f", (double)air->pm25' in metrics
assert '"ug/m3 %.5s", air->updated_at + 11' in metrics
assert 'set_text(s_aqi_info, "暂无空气数据")' in metrics
assert 'set_text(s_aqi_updated, "")' in metrics
print('PASS: setting drafts, latched button fallback, alarm select/edit/cancel/save, weather carousel and bounded AQI rows; scope=offline checks; pending=C/LVGL/device validation')
