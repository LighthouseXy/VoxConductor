"""Read-only V3 design/input/audio boundaries. No build, API or hardware access."""
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
ui = (MAIN / "app_ui.c").read_text()
theme = (MAIN / "app_ui_theme.h").read_text()
desktop = (MAIN / "app_desktop.c").read_text()
main = (MAIN / "main.c").read_text()
audio = (MAIN / "app_audio.c").read_text()
agent = (MAIN / "app_agent.c").read_text()

for text in (ui, desktop, theme):
    assert "lv_canvas" not in text and "heap_caps_malloc" not in text
    assert "bg_grad" not in text and "transform_scale" not in text
assert all(color in theme for color in ("0xFFF1C9", "0xFFC66E", "0xFF9B7D", "0xAFE3C9"))
assert "holo_surface" in ui and "holo_surface" in desktop
assert 'set_text(s_time_label, "--:--")' in ui
assert '"%H:%M"' in ui and '"%H:%M:%S"' not in ui
assert "&app_ui_font_digits_56" in ui and "&app_ui_font_16" in desktop
assert "#define MIC_UI_REFRESH_MS 100" in ui and "s_mic_bars[5]" in ui
assert "atomic_load(&s_mic_level)" in ui and "weights[5]" in ui
assert "holo_robot_pose" in ui and "s_wait_dots[3]" in ui
assert "app_desktop_clock_text" not in ui
assert "network.weather_error != ESP_OK || age > 1200" in ui
assert "humidity_percent" in ui and "humidity_percent" in agent
allowed = ui.split("bool app_ui_voice_allowed(void)", 1)[1].split("bool app_ui_take_retry(void)", 1)[0]
assert "s_current_page_index == 2" in allowed and "!s_desktop_open && !s_detail_open" in allowed
wake = main.split("static void handle_wake(void)", 1)[1].split("static void wake_dispatch_task", 1)[0]
assert "app_ui_wake_voice()" in wake and "s_voice_turn_active" in wake
route = main.split("static bool handle_button_event(", 1)[1].split("static esp_err_t run_voice_turn(", 1)[0]
assert "app_ui_handle_long_press()" in route and "app_desktop_button_released()" in route
assert "app_ui_take_retry()" in route and "s_retry_after_release" in route
assert "s_retry_requested = s_error_selection == 0" in ui
tool_items = re.search(r'tool_items\[\]\s*=\s*\{(.*?)\};', desktop, re.S)
assert tool_items and re.findall(r'"([^"\\]*)"', tool_items.group(1)) == ["番茄钟", "背单词", "任务", "媒体", "测反应", "弹球", "太空飞机"]
assert "return sizeof(tool_items) / sizeof(*tool_items);" in desktop
assert "s_settings_from_root" not in desktop and "void app_desktop_open_settings(void)" in desktop
assert "app_desktop_open_settings()" in ui
assert "atomic_load(&s_wait_release)" in desktop
assert "MENU_ARCADE" in desktop and "app_games_close()" in desktop
assert "legacy_preferences_t" in desktop and "reserved_legacy[9]" in desktop
assert "tts_rate_percent, tts_voice_id, reserved_legacy[16]" in desktop
assert ".version = 5" in desktop and "s_preferences_writable = false" in desktop
assert "loaded.version == 3 || loaded.version == 4 || loaded.version == 5" in desktop
assert "loaded.tts_rate_percent = 100" in desktop and "loaded.tts_voice_id = 0" in desktop
assert "app_audio_game_feedback" not in desktop and "i2s_" not in desktop
worker = main.split("static void wake_dispatch_task", 1)[1].split("static void reminder_task", 1)[0]
assert "dispatch_wake()" in worker
assert "app_audio_" not in worker and "app_desktop_" not in worker
reminder = audio.split("esp_err_t app_audio_local_reminder(void)", 1)[1].split("static void init_microphone", 1)[0]
assert "game_feedback" not in reminder and "app_audio_play_mono_pcm" in reminder
assert not (MAIN / "app_games_extra.c").exists()
assert (MAIN / "app_games_core.c").exists() and (MAIN / "app_games.h").exists()
assert "render_grid()" in desktop and "s_grid_tiles[4]" in desktop
assert "lv_qrcode_create(screen)" in desktop
assert 's_hint = holo_label(' in desktop
reopen = agent.split("bool app_agent_task_change(", 1)[1].split("bool app_agent_due_notice(", 1)[0]
assert 'bool reopen = action && !strcmp(action, "reopen")' in reopen
assert 'due_value(item) > time(NULL)' in reopen
font = (MAIN / "app_ui_font_digits_56.c").read_text()
glyphs = {int(x, 16) for x in re.findall(r"/\* U\+([0-9a-fA-F]+)", font)}
assert {ord(c) for c in "0123456789:-°"} <= glyphs
print('PASS: transparent UI, bounded arcade catalog, parent/input/audio boundaries, fonts and migration guards; scope=offline checks; pending=C/runtime/hardware validation')
