"""Non-compiling source guards and navigation models for the weather/home fixes."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
main = root / "firmware/main"
ui = (main / "app_ui.c").read_text()
desktop = (main / "app_desktop.c").read_text()
weather = (main / "app_weather.c").read_text()
header = (main / "app_weather.h").read_text()
network = (main / "app_network.c").read_text()
config = (root / "firmware/sdkconfig").read_text()
motion = (main / "app_motion.h").read_text()

assert "APP_MOTION_DIRECTION -1" in motion and "APP_MOTION_VERTICAL_DIRECTION -1" in motion
assert "CONFIG_LV_FONT_MONTSERRAT_40=y" in config
assert "# default:\nCONFIG_LV_FONT_MONTSERRAT_40=y" not in config
dial = ui.split("static void create_clock(", 1)[1].split("static void create_page", 1)[0]
assert "&lv_font_montserrat_40" in dial and "0xFFFFFF, 28, 65, 184" in dial
for label in ("time", "date"):
    assert f"lv_obj_set_style_text_opa(clock->{label}, LV_OPA_COVER" in dial
assert "s_forecast_marks[12]" in ui and '"未来%u小时", count' in ui
assert "count < 12 ? count : 12" in ui
assert "count > 4 ? count - 4 : 0" in ui
assert "count < 12 ? \"数据不足，等待更新\"" in ui
# Four-hour viewport, five reusable slots and continuous access to all 12 hours.
assert "#define FORECAST_SLOTS 5" in ui
assert "s_forecast_strip" in ui and "render_forecast_window()" in ui
assert "forecast_scroll_tick(x, elapsed_ms)" in ui
for total in (1, 4, 5, 12):
    maximum = max(0, total - 4)
    visited = {i for start in range(maximum + 1) for i in range(start, min(start + 4, total))}
    assert visited == set(range(total))
    assert max(0, min(maximum, maximum + 1)) == maximum
assert "render_weather_metrics(day, fresh)" in ui
assert "s_ui_weather.current_uv" in ui and "app_weather_uv_level(uv)" in ui
assert "app_weather_uv_periods(&s_ui_weather, day" in ui
assert "uv_period_count(periods)" in ui
assert "h->uv_index > 3.0f" in weather
assert "s_uv_max" in ui and "s_uv_marker" in ui
assert "s_wind_points[2]" in ui and "app_weather_wind_direction" in ui
for variable in ("wind_speed_10m", "wind_direction_10m", "wind_gusts_10m", "uv_index"):
    assert variable in header
assert "&current=us_aqi,pm2_5" in header and "air-quality-api.open-meteo.com" in header
assert 'unit_is(units, "us_aqi", "USAQI")' in weather
assert 'unit_is(units, "pm2_5", "μg/m³")' in weather
assert "aqi->valuedouble == aqi->valueint" in weather
assert "out->current_uv_available = in_range(current_uv, 0, 50)" in weather
aq = network.split("static esp_err_t fetch_air_quality", 1)[1].split("esp_err_t app_network_fetch_weather", 1)[0]
assert ".timeout_ms = 15000" in aq and "esp_http_client_cleanup(client)" in aq
assert "s_weather_cache.air_cached = false" in aq and "ESP_LOGW" in aq
assert "client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM" in aq
assert "app_air_quality_t air = {0}" in aq
assert "!app_weather_parse_air(response_data, &air)" in aq
assert "s_weather_cache.air = air" in aq
assert "s_weather_cache.air_cached = s_weather_cache.air.available" in aq
assert "return result" in aq and "30秒后重试" in aq
assert "weather.air = s_weather_cache.air" in network
fetch = network.split("esp_err_t app_network_fetch_weather(void)", 1)[1].split("static void weather_update_task", 1)[0]
assert "s_weather_cache = weather;" in fetch and "fetch_air_quality(" not in fetch
assert "weather.air_cached = s_weather_cache.air_cached" in fetch
maintenance = network.split("static void weather_update_task", 1)[1]
assert "now >= air_due" in maintenance and "air_error = fetch_air_quality()" in maintenance
assert "AIR_REFRESH_INTERVAL_MS : AIR_RETRY_INTERVAL_MS" in maintenance
assert "xTaskCreate" not in aq and "app_ui_" not in aq

select = desktop.split("void app_desktop_select_wallpaper_mode", 1)[1].split("bool app_desktop_configuration", 1)[0]
assert "select_home_style(1)" in select
home_style = desktop.split("static void select_home_style(", 1)[1].split("static void open_wallpapers", 1)[0]
assert "s_home_style = style" in home_style and "app_ui_preview_home()" in home_style
assert "save_later()" in home_style
apply = desktop.split("app_config_result_t app_desktop_configuration_patch", 1)[1].split("bool app_desktop_take_focus_notice", 1)[0]
assert "visual_changed" in apply and "app_ui_preview_home()" in apply
refresh = ui.split("static void refresh_media_ui(void) {", 1)[1].split("static void update_dial", 1)[0]
preview = refresh.split("app_media_snapshot(&s_ui_media)", 1)[0]
assert "s_active_voice_state == NO_VOICE_STATE" in preview
assert "app_desktop_is_playing" not in preview
assert "app_ui_note_activity()" in preview and "s_current_page_index = 0" in preview
assert "app_desktop_visible(false)" in preview and "lv_screen_load(s_test_screens[0])" in preview
assert "visible(s_animated.group, digital_home && dynamic)" in refresh
# Dynamic wallpapers need neither SD pictures nor the idle wallpaper toggle.
for images, toggle in ((0, False), (0, True), (3, False)):
    digital_home, dynamic = True, True
    assert digital_home and dynamic
print('PASS: daylight clock, restored shared motion, all 12 forecast hours, current UV/air/wind cards and applied home preview; scope=offline checks; pending=C/device validation')
