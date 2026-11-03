"""Source guards and a Python tilt/time/window model.

C/LVGL execution and physical device frame timing require separate validation."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
ui = (root / "firmware/main/app_ui.c").read_text()
main = (root / "firmware/main/main.c").read_text()

def constant(name):
    return int(re.search(rf"^#define {name} (\d+)", ui, re.M)[1])

column = constant("FORECAST_COLUMN_PX")
slots = constant("FORECAST_SLOTS")
deadzone = constant("FORECAST_DEADZONE_MG")
full_tilt = constant("FORECAST_FULL_TILT_MG")
slow = constant("FORECAST_MIN_SPEED")
fast = constant("FORECAST_MAX_SPEED")
assert column == 57 and slots == 5 and 0 < deadzone < full_tilt
assert 0 < slow < fast <= 300

def speed(tilt):
    magnitude = abs(tilt)
    if magnitude <= deadzone:
        return 0
    magnitude = min(magnitude, full_tilt)
    value = slow + (magnitude - deadzone) * (fast - slow) // (full_tilt - deadzone)
    return -value if tilt < 0 else value

def advance(offset, tilt, elapsed, count, valid=True, vertical=False, clicking=False):
    if not valid or vertical or clicking or not count:
        return offset
    limit = max(0, count - 4) * column * 1000
    return max(0, min(limit, offset + speed(tilt) * min(elapsed, 100)))

assert all(speed(x) == 0 for x in range(-deadzone, deadzone + 1))
assert speed(deadzone + 1) > 0
assert speed(full_tilt) == speed(2000) == fast
assert all(speed(-x) == -speed(x) for x in range(0, 2001))
assert all(speed(x) <= speed(x + 1) for x in range(0, full_tilt))
assert advance(0, 0, 33, 12) == 0
assert advance(0, full_tilt, 2000, 12) == fast * 100  # bounded catch-up
for count in range(1, 13):
    limit = max(0, count - 4) * column * 1000
    offset = 0
    for _ in range(1000):
        offset = advance(offset, full_tilt, 33, count)
    assert offset == limit
    for options in ({"valid": False}, {"vertical": True}, {"clicking": True}):
        assert advance(offset, -full_tilt, 33, count, **options) == offset
    for _ in range(1000):
        offset = advance(offset, -full_tilt, 33, count)
    assert offset == 0
    visited = set()
    for first in range(max(0, count - 4) + 1):
        window = list(range(first, min(first + slots, count)))
        assert len(window) <= 5 and all(0 <= i < count for i in window)
        visited.update(window)
    assert visited == set(range(count))

formula = ui.split("static int forecast_speed(int tilt)", 1)[1].split("static void forecast_scroll_tick", 1)[0]
assert "magnitude <= FORECAST_DEADZONE_MG" in formula
assert "magnitude = FORECAST_FULL_TILT_MG" in formula
assert "(magnitude - FORECAST_DEADZONE_MG) * (FORECAST_MAX_SPEED - FORECAST_MIN_SPEED)" in formula
assert "return tilt < 0 ? -speed : speed" in formula
scroll = ui.split("static void forecast_scroll_tick", 1)[1].split("static unsigned uv_period_count", 1)[0]
assert "elapsed_ms = 100" in scroll and "if (next < 0) next = 0" in scroll
assert "if (next > limit) next = limit" in scroll
assert "first != s_forecast_first" in scroll and "render_forecast_window()" in scroll
assert "lv_obj_scroll_to_x" in scroll and "LV_ANIM_OFF" in scroll
tick = ui.split("static void detail_motion_tick", 1)[1].split("void app_ui_enter_page", 1)[0]
assert "s_active_voice_state != NO_VOICE_STATE" in tick
assert "valid && abs(y) <= MOTION_CENTER_MG" in tick
assert "!app_button_click_pending()" in tick
assert "forecast_scroll_tick(x, elapsed_ms)" in tick
assert "lv_obj_set_scrollbar_mode(s_forecast_group, LV_SCROLLBAR_MODE_OFF)" in ui
assert "s_forecast_shapes[FORECAST_SLOTS][10]" in ui
assert "s_forecast_shapes[12]" not in ui  # Don't create all 120 icon shapes.
refresh = ui.split("static void refresh_page_details", 1)[1].split("bool app_ui_detail_motion", 1)[0]
assert "!s_weather_detail_dirty" in refresh and "!memcmp(&s_detail_weather, &s_ui_weather" in refresh
assert "cached == s_weather_detail_cached" in refresh
assert refresh.index("return;") < refresh.index("render_weather_metrics(day, fresh)")
assert "visible(s_metrics_group, false)" not in refresh

# The actual installed LVGL built-in formatter doesn't handle floats; weather
# now shares libc formatting with change-only label updates.
formatter = ui.split("static void set_textf(lv_obj_t *label, const char *format, ...) {", 1)[1].split("#define WALL_PARTICLES", 1)[0]
assert "vsnprintf(text, sizeof(text), format, args)" in formatter
assert "set_text(label, text)" in formatter
assert "if (strcmp(lv_label_get_text(label), text))" in ui
assert "lv_label_set_text_fmt(" not in ui
metrics = ui.split("static void render_weather_metrics", 1)[1].split("static void refresh_page_details", 1)[0]
for label in ("s_uv_value", "s_uv_max", "s_aqi_info", "s_wind_info", "s_wind_gust"):
    assert f"set_textf({label}," in metrics
assert "s_uv_period_title" not in ui
assert 'set_textf(s_uv_period, ">3 %s", line)' in metrics
assert "逐时数据不完整" in (root / "firmware/main/app_weather.c").read_text()
layout = ui.split("static void create_weather_metrics", 1)[1].split("esp_err_t app_ui_init", 1)[0]
assert "i / 2 ? 138 : 36" in layout and "i / 2 ? 98 : 96" in layout
assert "130, 112, 96" in layout  # UV period is inside the upper-right card.
assert "8, 219, 224" not in layout and "8, 200, 224" not in layout
# Concrete formatting expectation, independent of the LVGL formatter.
assert f"{5.4:.1f}" == "5.4" and f"{0.0:.1f}" == "0.0"
assert f"PM2.5 {49.0:.0f}" == "PM2.5 49"
assert 'set_textf(s_aqi_updated, "ug/m3 %.5s"' in metrics
assert f"{12.0:.0f} km/h" == "12 km/h"
assert "xTaskCreate" not in main.split("static void dispatch_motion", 1)[1].split("static void update_mic_level", 1)[0]
print('PASS: tilt-speed/deadzone/time clamp/window model, delta rendering and numeric/UV-card source guards; scope=offline checks; pending=C/LVGL/device validation')
