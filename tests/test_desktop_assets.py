"""Source and asset checks; firmware and hardware acceptance are separate tasks."""
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
FILES = ["app_desktop.c", "app_control.c", "app_focus.c", "app_wake.c", "main.c", "app_audio.c", "app_motion.c", "app_ui.c", "app_network.c", "app_media.c", "app_media_web.c"]
FILES.append("app_wake_diagnostic.c")
FILES.extend(["app_turn_log.c", "app_cloud.c", "app_cloud_asr.c", "app_cloud_tts.c", "app_storage.c"])
strings = re.compile(r'"(?:\\.|[^"\\])*"')
tokens = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for name in FILES:
    text = tokens.sub(lambda m: " " * len(m[0]), (MAIN / name).read_text())
    stack = []
    for c in text:
        if c in "({[":
            stack.append(c)
        elif c in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[c], name
    assert not stack, name
agent = (MAIN / "app_agent.c").read_text()
literal_json = re.compile(r'cJSON_Parse\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', re.S)
schemas = [json.loads("".join(json.loads(s[0]) for s in strings.finditer(m[1]))) for m in literal_json.finditer(agent)]
assert schemas and any(isinstance(s, list) and any(isinstance(v, dict) and v.get("function", {}).get("name") == "prepare_task_change" for v in s) for s in schemas)
pcm = (MAIN / "reminder.pcm").read_bytes()
assert len(pcm) % 2 == 0 and 48000 <= len(pcm) <= 480000
samples = struct.unpack("<" + "h" * (len(pcm) // 2), pcm)
assert max(abs(s) for s in samples) > 500 and sum(abs(s) for s in samples) / len(samples) > 50
font_paths = [MAIN / "app_ui_font_14.c", ROOT / "firmware/managed_components/lvgl__lvgl/src/font/lv_font_source_han_sans_sc_14_cjk.c"]
glyphs = {int(x, 16) for path in font_paths for x in re.findall(r"/\* U\+([0-9a-fA-F]+)", path.read_text())}
required = set()
for name in ["app_desktop.c", "app_words.c", "app_games.c", "app_control.c", "main.c", "app_ui.c", "app_media.c", "app_media_web.c"]:
    source = (MAIN / name).read_text()
    if name == "app_control.c":
        # Spoken exit aliases are ASR input, never rendered as UI labels.
        before, rest = source.split("bool app_control_end_session", 1)
        source = before + rest[rest.index("static bool volume_number"):]
    # Serial logs and these HTTP-only diagnostics never reach LVGL fonts.
    source = re.sub(r'(?:ESP_LOG[EWIDV]|APP_LOG_DETAIL)\((?:[^;"]|"(?:\\.|[^"\\])*")*\);', "", source)
    if name == "app_media_web.c":
        before, rest = source.split("static esp_err_t wake_diagnostic_handler", 1)
        source = before + rest[rest.index("static void add_files"):]
    for literal in strings.finditer(source):
        required.update(c for c in literal[0] if ord(c) >= 0x3000)
assert not (required - {chr(c) for c in glyphs}), "New labels need regenerated font glyphs"
large_font = MAIN / "app_ui_font_20.c"
large_glyphs = {int(x, 16) for x in re.findall(r"/\* U\+([0-9a-fA-F]+)", large_font.read_text())}
assert not (required - {chr(c) for c in large_glyphs}), "Large selected/title font is missing labels"
medium_font = MAIN / "app_ui_font_16.c"
medium_glyphs = {int(x, 16) for x in re.findall(r"/\* U\+([0-9a-fA-F]+)", medium_font.read_text())}
assert not (required - {chr(c) for c in medium_glyphs}), "Helper font is missing labels"
desktop = (MAIN / "app_desktop.c").read_text()
tick = desktop[desktop.index("static void tick("):desktop.index("void app_desktop_init(")]
assert "nvs_" not in tick and "app_agent_" not in tick and "app_cloud_" not in tick
assert "app_games_open" in desktop and "app_games_close" in desktop
assert "MENU_VOLUME" in desktop and "s_grid_tiles[4]" in desktop
ui = (MAIN / "app_ui.c").read_text()
ui_tick = ui[ui.index("static void update_home_time("):ui.index("esp_err_t app_ui_init(")]
assert "app_network_status_snapshot(&network)" in ui_tick and "render_weather(&s_ui_weather)" in ui_tick
assert "校时重试" in ui_tick and "weather_retry_seconds" in ui_tick
assert "app_ui_set_weather" not in (MAIN / "app_network.c").read_text()
render = ui[ui.rindex("static void render_weather("):ui.index("static void update_home_time(")]
assert "lvgl_port_lock" not in render, "Weather rendering belongs to the LVGL timer"
assert (MAIN / "app_games_core.c").exists()
assert "app_games_close()" in desktop
main = (MAIN / "main.c").read_text()
assert "app_cloud_local_ready()" in main and "app_audio_local_reminder()" in main
assert "default n" in (MAIN / "Kconfig.projbuild").read_text()
assert "CONFIG_VOX_WAKE_WORD=y" in (ROOT / "firmware/sdkconfig").read_text()
wake = (MAIN / "app_wake.c").read_text()
assert "producer_generation != current" in wake and "atomic_fetch_add(&generation, 1)" in wake
assert "action_valid(action->valuestring)" in agent and "app_agent_request_matches(args, user_text)" in agent
assert "rule_changed" in agent and "next_workday(due)" in agent
cmake = (MAIN / "CMakeLists.txt").read_text()
assert all(f'"{name}"' in cmake for name in ["app_desktop.c", "app_control.c", "app_focus.c", "app_wake.c", "reminder.pcm", "app_ui_font_20.c", "app_ui_font_16.c", "app_ui_font_digits_56.c"])
print(f"PASS: desktop source boundaries, JSON literals, glyphs and local PCM ({len(pcm)/48000:.2f}s); C compilation and hardware NOT run")
