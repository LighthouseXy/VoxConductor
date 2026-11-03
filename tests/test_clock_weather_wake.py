"""Read-only source/asset guards for clock, alarm, forecast and wake integration.

Scope: source and Python models. Behavioral C fixtures in test_weather.c and
test_agent.c provide separately runnable host checks."""
import array
import csv
import hashlib
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
ui = (MAIN / "app_ui.c").read_text()
desktop = (MAIN / "app_desktop.c").read_text()
agent = (MAIN / "app_agent.c").read_text()
weather = (MAIN / "app_weather.c").read_text()
wh = (MAIN / "app_weather.h").read_text()
wake = (MAIN / "app_wake.c").read_text()
config = (MAIN / "Kconfig.projbuild").read_text()
main = (MAIN / "main.c").read_text()

assert "lv_arc_create(screen)" in ui and "lv_line_set_points" in ui
assert "lv_point_precise_t points[3][2]" in ui
assert "lround(cos(radians)" in ui and "lround(sin(radians)" in ui
assert '"%02d/%02d %s"' in ui and '"%H:%M"' in ui

# Shared dial, optional digital wallpaper home; retain input and timing.
page = ui.split("static void create_page(", 1)[1].split("static void create_voice_overlay", 1)[0]
home = page.split("// Optional wallpaper + digital time home", 1)[1].split("} else if (index == 1)", 1)[0]
assert "&app_ui_font_digits_56" in home and "20, 75, 200" in home
assert "20, 139, 200" in home and "s_clock_hands" not in home
assert re.search(r"if \(index >= 2\)\s+holo_label\(screen, PAGE_TITLES\[index\]", page)
dial = ui.split("static void create_clock(", 1)[1].split("static void create_page", 1)[0]
assert "clock->time = holo_label" in dial and "clock->date = holo_label" in dial
assert "clock->hands[i] = lv_line_create(screen)" in dial
assert dial.index("clock->depth = holo_rect") < dial.index("clock->hands[i] = lv_line_create")
assert "20, 27, 200, 200" in dial and "&lv_font_montserrat_40" in dial
assert "lv_obj_set_style_text_opa(clock->time, LV_OPA_COVER" in dial
assert "lv_obj_set_style_text_opa(clock->date, LV_OPA_COVER" in dial
assert "s_time_label" not in dial and "s_date_label" not in dial
init = ui.split("esp_err_t app_ui_init(void)", 1)[1]
assert "create_clock(screen, &s_clocks[0])" in page
assert page.index("s_wallpaper = lv_image_create") < page.index("create_clock(screen, &s_clocks[0])") < page.index("s_home_digital = holo_rect")
assert init.index("create_clock(s_idle_screen, &s_clocks[1])") < init.index("update_home_time(NULL)")
assert "按键或倾斜唤醒" not in ui
assert "lv_timer_create(update_home_time, 1000" in init
update = ui.split("static void update_home_time(", 1)[1].split("static void discard_unpublished_images", 1)[0]
assert "update_dial(&s_clocks[i], NULL, lv_label_get_text(s_date_label))" in update
assert "update_dial(&s_clocks[i], &local, NULL)" in update
assert 'set_textf(s_date_label, "%02d月%02d日  %s"' in update
assert 'set_textf(clock->date, "%02d/%02d %s"' in ui
assert 'nvs_get_u32(h, "home_style", &home_style)' in desktop
assert 'nvs_set_u32(h, "home_style", home_style)' in desktop
assert ".version = 5" in desktop
assert "loaded.version == 3 || loaded.version == 4 || loaded.version == 5" in desktop
assert "sizeof(preferences_t) == sizeof(preferences_v4_layout_t)" in desktop
assert 'home_style_names[] = {"圆形时钟", "壁纸+数字时间"}' in desktop
assert "select_home_style((unsigned)s_selection)" in desktop
assert "visible(s_clocks[0].group, !digital_home)" in ui
assert "lv_obj_set_style_translate_x(clock->depth, 3 + dx" in ui
footer = ui.split("static void page_footer(", 1)[1].split("static void error_selection", 1)[0]
assert "90 + i * 18, 235, i == page ? 12 : 5, 5" in footer
assert 235 + 5 == 240  # Bottom row stays within the visible panel.
assert "app_desktop_open_clock()" in ui
assert "MENU_ALARM" in desktop and "MENU_CLOCK_TASKS" not in desktop
assert '"once", "daily", "weekdays"' in desktop
assert re.search(r"motion_gesture_step\(\s*&s_alarm_vertical", desktop)
tick = desktop.split("static void tick(", 1)[1].split("void app_desktop_init(", 1)[0]
assert "app_agent_create_alarm" not in tick and "xQueueSend" not in tick
service = desktop.split("void app_desktop_service(void)", 1)[1]
assert "app_agent_create_alarm" in service and "command.repeat < 3" in service
alarm = agent.split("bool app_agent_create_alarm(", 1)[1].split("unsigned app_agent_task_snapshot", 1)[0]
create=agent.split("bool app_agent_create_local_task(",1)[1].split("unsigned app_agent_task_snapshot",1)[0]
assert "save_tasks(tasks)" in create and "esp_random()" in create
assert "cJSON_GetArraySize(tasks)>=8" in create and "due<=now" in create
assert re.search(r"hour\s*>\s*23",alarm) and re.search(r"minute\s*>\s*59",alarm)
assert "app_cloud_" not in alarm and "app_agent_execute" not in alarm

assert "forecast_days=2" in wh and "uv_index_max" in wh
assert "APP_WEATHER_HOURS 48" in wh
assert "static EXT_RAM_BSS_ATTR char response_data[8192]" in (MAIN / "app_network.c").read_text()
assert "valid_temperature(cJSON_GetArrayItem(temps, i))" in weather
assert "cJSON_GetArraySize(rain) == hours" in weather
assert "day <= limit" in weather and "year % 400 == 0" in weather
assert "h->uv_index > 3.0f" in weather and "seen != 0xFFFFFFu" in weather
assert "mktime(&t) + 86400" in weather
assert "app_ui_detail_motion(direction)" in main
assert "app_weather_future_hours" in ui and "app_weather_uv_periods" in ui
assert "!strcmp(s_ui_weather.uv_day, day)" in ui
assert "displayed_s_ui_weather" not in ui
assert "s_ui_s_ui_weather" not in ui
assert "static EXT_RAM_BSS_ATTR app_weather_t s_ui_weather" in ui
assert "uv_period_count(periods)" in ui and "s_weather_uv_group" in ui
assert "lv_timer_create(detail_motion_tick, 33" in ui
assert 'title = s_weather_uv_page ? "天气参数" : "未来12小时"' in ui
assert "return count < 12 ? count : 12" in ui
assert "hour[14] = hour[15] = '0'" in ui
assert "#define FORECAST_SLOTS 5" in ui
assert "s_forecast_shapes[FORECAST_SLOTS][10]" in ui and "s_forecast_temp[FORECAST_SLOTS]" in ui
assert "count > 4 ? count - 4 : 0" in ui
assert "forecast_scroll_tick(x, elapsed_ms)" in ui
assert "lv_obj_scroll_to_x(s_forecast_group" in ui and "LV_ANIM_OFF" in ui
assert "s_weather_hour_selection" not in ui  # No discrete jump or wrap to old hours.
assert "forecast_slide_x" not in ui
assert "app_agent_request_matches" in agent
assert 'cJSON_CreateString("note")' in agent
assert 'cJSON_IsString(kind) && !strcmp(kind->valuestring, "note")' in agent
assert 'strstr(user_text, title->valuestring) != NULL' in agent
assert '"tool_choice", "auto"' in (MAIN / "app_cloud.c").read_text()
assert '备忘录已保存：%s。' in agent and 'command_is(text, "查看备忘录")' in agent
assert 'memo ? task->title : name' in desktop

assert '#define APP_WAKE_PHRASE "小冰小冰"' in (MAIN / "app_wake.h").read_text()
assert "CONFIG_VOX_WAKE_MODEL_NAME" in wake and "strcmp(models->model_name[i]" in wake
assert "esp_srmodel_filter" not in wake  # no first/unrelated model fallback
assert 'default "wn9_xiaobinxiaobin_tts"' in config and "wn9_customword" in config
sdkconfig = (ROOT / "firmware/sdkconfig").read_text()
assert "CONFIG_VOX_WAKE_WORD=y" in sdkconfig
assert 'CONFIG_VOX_WAKE_MODEL_NAME="wn9_xiaobinxiaobin_tts"' in sdkconfig
assert re.findall(r"^CONFIG_SR_WN_(\w+)=y$", sdkconfig, re.M) == ["WN9_XIAOBINXIAOBIN_TTS"]
assert 'CONFIG_ESP_SR_EXTERNAL_MODEL_PATH=""' in sdkconfig
assert "CONFIG_VOX_ACOUSTIC_BARGE_IN=y" in sdkconfig
assert 'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions_wake.csv"' in sdkconfig
assert 'CONFIG_PARTITION_TABLE_FILENAME="partitions_wake.csv"' in sdkconfig
for symbol in ("VOX_WAKE_WORD", "SR_WN_WN9_XIAOBINXIAOBIN_TTS", "PARTITION_TABLE_CUSTOM_FILENAME"):
    assert f"# default:\nCONFIG_{symbol}=" not in sdkconfig
model = ROOT / "firmware/managed_components/espressif__esp-sr/model/wakenet_model/wn9_xiaobinxiaobin_tts"
for file in ("_MODEL_INFO_", "wn9_data", "wn9_index"):
    assert (model / file).stat().st_size > 0
assert "小冰小冰".encode("utf-8") in (model / "_MODEL_INFO_").read_bytes()
# Keep saved preferences and the app offset intact; only add the model partition.
def partitions(filename):
    lines = (ROOT / "firmware" / filename).read_text().splitlines()
    return [[field.strip() for field in row] for row in csv.reader(
        line for line in lines if line.strip() and not line.lstrip().startswith("#"))]
old_parts = partitions("partitions.csv")
new_parts = partitions("partitions_wake.csv")
assert new_parts[:-1] == old_parts
assert new_parts[-1][:5] == ["model", "data", "spiffs", "0x410000", "3M"]
assert 'CONFIG_ESPTOOLPY_FLASHSIZE="16MB"' in sdkconfig
assert 0x410000 + 3 * 1024 * 1024 <= 16 * 1024 * 1024
# Boot must open idle detection without requiring a first button conversation.
boot = main.split("void app_main(void)", 1)[1]
assert boot.index("app_wake_init(handle_wake)") < boot.index("atomic_store_explicit(&s_voice_ready") < boot.index("restore_wake();")
assert "app_wake_set_enabled(s_preferences.wake_enabled)" in desktop
assert '"本地唤醒"' not in desktop
assert "app_wake_set_enabled(s_preferences.wake_enabled)" in desktop
assert "app_ui_wake_voice()" in main and "start_voice(true)" in main
assert "wn->get_samp_rate(model) != 16000" in wake
assert "wn->get_channel_num(model) != 1" in wake

backup = ROOT / "backups/clock-ui-20261005"
expected = {
    "app_ui.c": "abbf697b6ff510682b495139d29d17337399e568f464eb5368578eb95bba8bba",
    "app_ui.h": "21952b4df21c1cf6f0034205235053759e84b3a5c137aa380870f49c00271b10",
    "app_ui_theme.h": "fb8f58b0b4469765ec1b9fa2f53b99421561fdedd4d722d8a20e0e8abf29d2dd",
    "app_desktop.c": "78d896bd18c2827fac4ea4af82a96515cd956d1bda99019e3d583bd6ac0c0f40",
    "app_desktop.h": "4eba4692c7a5d7fd965694284dd968d60cf54893d6f03ea01d7667dd34f0c511",
}
for file, sha in expected.items():
    assert hashlib.sha256((backup / file).read_bytes()).hexdigest() == sha, file

pack = ROOT / "assets/media-pack"
catalog = json.loads((pack / "catalog.json").read_text())
for item in catalog["wallpapers"]:
    data = array.array("H")
    data.frombytes((pack / "sdcard/media/images" / item["file"]).read_bytes())
    assert len(data) == 240 * 240
    assert sum(value == 0 for value in data) / len(data) > 0.55
    central = [data[y * 240 + x] for y in range(60, 160) for x in range(70, 170)]
    if item["file"][:7] not in ("wall_05", "wall_06"):
        assert sum(value == 0 for value in central) / len(central) > 0.95
    assert "transparent" in item["prompt"].lower()
    assert (pack / "generated-images" / item["source_image"]).is_file()
assert "Image.alpha_composite(black, rgba)" in (ROOT / "tools/prepare_media_pack.py").read_text()
assert "lv_obj_set_style_image_opa(s_wallpaper, LV_OPA_COVER" in ui

token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for file in ("app_ui.c", "app_desktop.c", "app_weather.c", "app_agent.c", "app_wake.c"):
    stack = []
    for char in token.sub("", (MAIN / file).read_text()):
        if char in "({[":
            stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char], file
    assert not stack, file
print('PASS: clock backup hashes, alarm/forecast/wake source guards, black wallpaper export; scope=offline checks; pending=C/build/device validation')
