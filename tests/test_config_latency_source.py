"""Offline structure and policy regression; no compiler, cloud or device access.

Source guards bind the policy models to implementation sites. The models do not
execute C and cannot establish real DNS/TLS duration, VAD accuracy or sound quality.
The separate test_holo_rework.py executes the actual embedded JS in a mocked DOM.
"""
import re
from copy import deepcopy
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
read = lambda name: (MAIN / name).read_text()
desktop, control, cloud = map(read, ("app_desktop.c", "app_control.c", "app_cloud.c"))
asr, runner, audio, report = map(read, ("app_cloud_asr.c", "main.c", "app_audio.c", "app_turn_log.c"))

patch = desktop.split("app_config_result_t app_desktop_configuration_patch(", 1)[1].split("bool app_desktop_configuration(", 1)[0]
assert patch.index("lvgl_port_lock(100)") < patch.index("configuration_locked()")
assert patch.index("merged.revision != expected_revision") < patch.index("MERGE(home_style")
assert patch.index("configuration_valid(&merged)") < patch.index("app_display_set_brightness") < patch.index("s_preferences.volume =")
assert patch.rindex("lvgl_port_unlock();", 0, patch.index("bool saved")) < patch.index("persist_settings(")
assert "mask & ~APP_CONFIG_ALL" in patch and "APP_CONFIG_SAVE_FAILED" in patch
assert "memcmp" not in desktop.split("static app_desktop_config_t configuration_locked", 1)[1].split("static bool configuration_valid", 1)[0]
assert "revision = esp_random() | 1u" in desktop  # reboot invalidates old browser baselines
table = control.split("static const config_field_t s_config_fields[]", 1)[1].split("#undef CONFIG_FIELD", 1)[0]
assert len(re.findall(r"CONFIG_FIELD\(", table)) == 8
for key in ("tts_rate_percent", "tts_voice_id", "work_minutes", "break_minutes"):
    assert "CONFIG_FIELD(" + key in table
configure = control.split("static cJSON *configure(", 1)[1].split("static cJSON *execute(", 1)[0]
assert "mask & field->mask" in configure and "mask & APP_CONFIG_WAKE" in configure
assert "(mask & legacy) != legacy" in configure and "expected_revision" in configure
settings = control.split("static cJSON *set_setting(", 1)[1].split("static cJSON *control_media", 1)[0]
assert "app_desktop_configuration_patch(NULL, &d, mask, d.revision)" in settings
assert settings.index("APP_CONFIG_CONFLICT") < settings.index("bool applied = readback")
assert '"tts_voice_id"' in settings and '"tts_rate_percent"' in settings

# Cancellation reaches header waits, body reads AND writes. No foreign task
# destroys the background client's handle while it is blocked in an SDK call.
assert "#define SUMMARY_CONNECT_TIMEOUT_MS 2000" in cloud
summary = cloud.split("static void summary_task(", 1)[1].split("void app_cloud_memory_tick", 1)[0]
assert "SUMMARY_CONNECT_TIMEOUT_MS, &s_summary_abort, &client" in summary
assert "app_cloud_http_write(client, body, strlen(body), &s_summary_abort)" in summary
assert "read_text(client, &job->result, &usage, NULL, &s_summary_abort)" in summary
response = cloud.split("static esp_err_t read_text(", 1)[1].split("esp_err_t app_cloud_transcribe", 1)[0]
transport = read("app_cloud_http.c")
assert "app_cloud_http_headers(client, abort, &status)" in response
assert "headers != -ESP_ERR_HTTP_EAGAIN" in transport
assert "esp_http_client_set_timeout_ms(client, 1000)" in transport
assert "app_cloud_http_cancelled(abort)" in response and "30000000" in response
assert asr.index("xTaskCreate(capture_task") < asr.index("app_cloud_summary_interrupt()")
assert "while (capturing && app_cloud_summary_busy()" in asr
assert "turn->summary_wait_us" in asr and "后台让出" in report
turn = runner.split("static esp_err_t run_voice_turn(", 1)[1].split("static void voice_conversation_task", 1)[0]
assert turn.index("app_cloud_listen(") < turn.index("heap_caps_free(recording)") < turn.index("app_cloud_ask(")
assert "recording = NULL" in turn
assert "CONFIG_VOX_AUTO_END_SILENCE_MS=800" in (ROOT / "firmware/sdkconfig").read_text()
assert "#define CONFIG_VOX_AUTO_END_SILENCE_MS 800" in audio
assert "stage = \"回答生成\"" in turn

# V5 reuses two unsigned slots, not a new serialized structure. Compare the
# declaration footprint to the preserved V3/V4 layout, including the shared
# tail. C _Static_assert checks actual size/offsets on the next authorized build.
layout = re.search(r"typedef struct \{\s*unsigned version, volume,\s*"
                  r"tts_rate_percent, tts_voice_id, reserved_legacy\[(\d+)\];"
                  r"(?P<tail>[^{}]*)\} preferences_t;", desktop, re.S)
old_layout = re.search(r"typedef struct \{\s*unsigned version, volume, "
                      r"reserved_legacy\[(\d+)\];(?P<tail>[^{}]*)"
                      r"\} preferences_v4_layout_t;", desktop, re.S)
assert layout and old_layout
assert 2 + int(layout[1]) == int(old_layout[1]) == 18
assert " ".join(layout["tail"].split()) == " ".join(old_layout["tail"].split())
assert "sizeof(preferences_t) == sizeof(preferences_v4_layout_t)" in desktop
for field in ("version", "volume", "wake_enabled", "focus", "brightness"):
    assert f"CHECK_PREF_OFFSET({field});" in desktop
loader = desktop.split("void app_desktop_init(void) {", 1)[1].split(
    "  if (s_preferences.focus.running)", 1)[0]
assert "size == sizeof(legacy_preferences_t)" in loader
assert "old.version == 1 || old.version == 2" in loader
assert "loaded.version == 3 || loaded.version == 4 || loaded.version == 5" in loader
assert "loaded.tts_rate_percent = 100" in loader and "loaded.tts_voice_id = 0" in loader
assert "s_preferences_writable = false" in loader and "nvs_flash_erase" not in loader

# Every compiled business module has a responsibility comment; generated font
# assets deliberately stay out of this coverage. Bracket checks are not a build.
cmake = read("CMakeLists.txt")
token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for filename in re.findall(r'"([^"/]+\.c)"', cmake):
    if filename.startswith("app_ui_font_"):
        continue
    source = read(filename)
    assert source.startswith("/*\n"), filename
    stack = []
    for char in token.sub("", source):
        if char in "({[": stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char], filename
    assert not stack, filename

# Model the CAS/partial-save contract, explicitly not native firmware execution.
class Config:
    def __init__(self):
        self.values = dict(volume=35, brightness=80, tts_rate_percent=100, tts_voice_id=0)
        self.disk = deepcopy(self.values)
        self.revision = 123
    def apply(self, edits, expected=0, save=True):
        if expected and expected != self.revision: return "conflict"
        merged = self.values | edits
        if not (0 <= merged["volume"] <= 100 and 0 <= merged["brightness"] <= 100
                and 50 <= merged["tts_rate_percent"] <= 200 and merged["tts_rate_percent"] % 5 == 0
                and 0 <= merged["tts_voice_id"] < 7): return "invalid"
        if merged != self.values: self.revision += 1
        self.values = merged
        if save: self.disk = deepcopy(merged)
        return "saved" if save else "partial"

c = Config(); baseline = c.revision
assert c.apply({"brightness": 19}, baseline) == "saved"
before = deepcopy(c.values)
assert c.apply({"volume": 50}, baseline) == "conflict" and c.values == before
assert c.apply({"volume": 50}, c.revision) == "saved" and c.values["brightness"] == 19
for invalid in (49, 101, 201):
    before = deepcopy(c.values)
    assert c.apply({"tts_rate_percent": invalid}, c.revision) == "invalid" and c.values == before
assert c.apply({"tts_voice_id": 3, "tts_rate_percent": 125}, c.revision) == "saved"
assert c.apply({"volume": 20}, c.revision, save=False) == "partial"
assert c.values["volume"] == 20 and c.disk["volume"] == 50
assert c.apply({"brightness": 90}) == "saved"  # compatibility caller, no fake CAS guarantee
assert c.values["tts_voice_id"] == 3 and c.values["tts_rate_percent"] == 125
print("PASS: config CAS/partial-save, shared voice fields, abort-aware summary, 800 ms tail, business-module comments; C/device NOT run")
