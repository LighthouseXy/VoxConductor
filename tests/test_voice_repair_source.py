"""Regression/source policies only. No C build, hardware, or paid API calls."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
control = (MAIN / "app_control.c").read_text()
desktop = (MAIN / "app_desktop.c").read_text()
main = (MAIN / "main.c").read_text()
cloud = (MAIN / "app_cloud.c").read_text()
asr = (MAIN / "app_cloud_asr.c").read_text()
acoustic = (MAIN / "app_acoustic.c").read_text()
sse = (MAIN / "app_sse.c").read_text()
network = (MAIN / "app_network.c").read_text()

# Receipts use readback, not a copy of the requested value, and retain failures.
settings = control.split("static cJSON *set_setting(", 1)[1].split("static cJSON *control_media", 1)[0]
assert "app_desktop_configuration(&actual, NULL)" in settings
assert "bool applied = readback && desktop_setting_value(&actual, s) == value" in settings
assert 'result(ok && applied, message)' in settings
assert '"applied", applied' in settings and '"saved", ok' in settings
assert '"value", desktop_setting_value(&actual, s)' in settings
assert '"value", value' not in settings
assert 'if (!strcmp(s, "volume")) return app_audio_get_volume();' in control
assert '"saved_volume", d.volume' in control
assert "if (s_menu == MENU_VOLUME && (mask & APP_CONFIG_VOLUME)) s_edit_value = set->volume" in desktop
assert 's_after.end_session = true' in settings and '"home"' in settings
assert '!strcmp(target, s_after.target)' in control  # duplicate scheduled open is idempotent
assert "s_wallpaper_mode = 0" not in settings  # restoring clock keeps selection

# Exact common controls must use the same request/range/receipt validator.
shortcut = control.split("static char *setting_answer(", 1)[1].split("static const char *command_start", 1)[0]
assert '"request", text' in shortcut and 'app_control_voice_execute("set_local_setting"' in shortcut
command = control.split("char *app_control_voice_command(", 1)[1].split("bool app_control_voice_finish", 1)[0]
assert 'setting_answer("home_style", 0, text)' in command
assert 'setting_answer("volume", value, text)' in command
assert 'volume_number(direct + n, &value)' in command
assert '你想把音量调到百分之多少' in command

# Mirror the strict number templates. Extra instructions/negation never match.
def number(text):
    text = re.sub(r"[ .!?\t\r\n。！？]+$", "", text)
    if text.startswith("百分之"):
        text = text[3:]
    text = text.removesuffix("%")
    if re.fullmatch(r"[0-9]+", text):
        value = int(text)
    elif text == "一百":
        value = 100
    else:
        digits = "零一二三四五六七八九"
        if text in digits and len(text) == 1:
            value = digits.index(text)
        elif re.fullmatch(r"[一二三四五六七八九]?十[一二三四五六七八九]?", text):
            tens, units = text.split("十")
            value = (digits.index(tens) if tens else 1) * 10 + (digits.index(units) if units else 0)
        else:
            return None
    return value if 0 <= value <= 100 else None

for text, expected in [("50", 50), ("百分之五十", 50), ("35%", 35), ("一百。", 100),
                       ("零", 0), ("十一", 11), ("101", None), ("50然后关闭唤醒", None),
                       ("50吗", None), ("不要调到50", None), ("-1", None), ("5.5", None)]:
    assert number(text) == expected

turn = main.split("static esp_err_t run_voice_turn(", 1)[1].split("static void voice_conversation_task(void *argument) {", 1)[0]
assert 'if (interrupted && app_control_strip_interrupt(transcript))' in turn
stop_only = turn.split('if (interrupted &&', 1)[1].split('app_acoustic_prepare()', 1)[0]
assert 'return ESP_OK' in stop_only and 'TURN_CANCEL' in stop_only  # no fabricated DONE
loop = main.split("static void voice_conversation_task(void *argument) {", 1)[1].split("void app_main", 1)[0]
assert 'interrupted = true' in loop and 'interrupted = false' in loop
assert 'app_network_voice_priority(true)' in loop and 'app_network_voice_priority(false)' in loop
assert network.count('!atomic_load(&s_voice_priority) &&') == 2
assert '小冰小冰 interruption accepted;' in acoustic and 'Speech suspected' not in acoustic
assert 'bool app_acoustic_listen' in acoustic and 'if (app_wake_is_enabled() && !app_acoustic_prepare())' in turn

assert 's->speech_committed && cJSON_IsArray(calls)' in sse
assert 's->speech_committed = true' not in cloud  # tool selection finishes before speech
assert 's->done &&' not in sse.split('static bool append', 1)[0]
assert 'strlen(s->text) >= 96' in sse  # short tool preambles stay buffered
assert 'if (n == -ESP_ERR_HTTP_EAGAIN) continue' in cloud
assert 'esp_http_client_set_timeout_ms(client, 1000)' in cloud
assert 'idle_deadline = esp_timer_get_time() + 30000000' in cloud
assert 'app_psram_zero(sizeof(*s))' in asr
assert 'app_psram_malloc(4097)' in asr and 'app_psram_malloc(4300)' in asr
assert 'summary_deadline = esp_timer_get_time() + 15000000' in asr
assert 'ASR timing:' in asr
assert 'app_cloud_begin_turn(&turn)' in turn

# Structural checks also catch accidental undefined local menu names.
enums = set(re.findall(r"^\s*(MENU_\w+)[,\s]", desktop.split("typedef struct", 1)[0], re.M))
assert set(re.findall(r"\bMENU_\w+\b", desktop)) <= enums
token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for source in (control, desktop, main, cloud, asr, acoustic, sse, network):
    stack = []
    for c in token.sub("", source):
        if c in "({[": stack.append(c)
        elif c in ")}]": assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[c]
    assert not stack
print("PASS: setting readback/home handoff/barge phrase/stream wait regression guards; device/native NOT run")
