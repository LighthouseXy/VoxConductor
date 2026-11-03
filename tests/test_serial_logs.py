"""Serial-log source guards and local policy models. C compilation and hardware acceptance run separately."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
read = lambda name: (MAIN / name).read_text()
header, turn, main, wake, tts, asr, cloud = map(read, (
    "app_log.h", "app_turn_log.c", "main.c", "app_wake.c",
    "app_cloud_tts.c", "app_cloud_asr.c", "app_cloud.c"))
sdk = (ROOT / "firmware/sdkconfig").read_text()
option = read("Kconfig.projbuild").split("config VOX_LOG_VERBOSE", 1)[1].split("config VOX_WAKE_WORD", 1)[0]
assert "default n" in option and "Detailed serial diagnostics" in option
assert "# CONFIG_VOX_LOG_VERBOSE is not set" in sdk.splitlines()
assert "#define APP_LOG_DETAIL(...) ESP_LOGI(__VA_ARGS__)" in header
assert "#define APP_LOG_DETAIL(...) ESP_LOGD(__VA_ARGS__)" in header
assert "suffix - id >= 8 ? suffix - 8 : id" in header
assert "malloc" not in header
boot = main.split("void app_main(void) {", 1)[1]
quiet = boot.split("#if !CONFIG_VOX_LOG_VERBOSE", 1)[1].split("#endif", 1)[0]
assert "esp_log_level_set(quiet_tags[i], ESP_LOG_WARN)" in quiet
assert boot.index("esp_log_level_set") < boot.index("app_control_init()")
assert not re.search(r'esp_log_level_set\(\s*"\*"', main)
assert "esp_log_set_vprintf" not in main + header

# Console changes preserve full stored IDs and persisted dialogue text.
emit = turn.split("static void emit(", 1)[1].split("void app_turn_emit(", 1)[0]
assert emit.index("app_storage_record_event(t, &event)") < emit.index("log_text(t, type, text)")
assert 'snprintf(s_source_turn, sizeof(s_source_turn), "%s", turn ? turn->id : "")' in cloud
assert "app_turn_event_t e = {turn->id" in cloud
logs = re.compile(r'(?:ESP_LOG[EWIDV]|APP_LOG_DETAIL)\((?:[^;"]|"(?:\\.|[^"\\])*")*\);')
for path in MAIN.glob("*.c"):
    assert "app_log_id(" not in logs.sub("", path.read_text()), path.name

# Model the preview's UTF-8 boundary/escaping policy; native execution is separate.
text_log = turn.split("static void log_text(", 1)[1].split("static void emit(", 1)[0]
compact = text_log.split("#if !CONFIG_VOX_LOG_VERBOSE", 1)[1].split("#else", 1)[0]
assert "if (type == TURN_ASSISTANT_TEXT)" in compact and "return;" in compact
capacity = int(re.search(r"char preview\[(\d+)\]", compact)[1])
assert capacity == 97 and "value[n] & 0xc0) == 0x80" in compact
assert text_log.index("value = preview") < text_log.index("cJSON_CreateString(value)")
for text in ("", "杭州天气怎么样？", "我" * 33, "a" * 95 + "界", "🙂" * 25,
             "\nE (0) fake\r\t" + "好" * 50, "a" * 96, "a" * 97):
    raw = text.encode()
    n = min(len(raw), capacity - 1)
    if len(raw) >= capacity:
        while n and raw[n] & 0xC0 == 0x80:
            n -= 1
    safe = json.dumps(raw[:n].decode(), ensure_ascii=False)
    assert len(json.loads(safe).encode()) < capacity
    assert all(c not in safe for c in "\n\r\t")

assert "detailed ? 2000000 : 15000000" in wake
assert "CONFIG_VOX_LOG_VERBOSE || app_wake_diagnostic_active()" in wake
assert "detailed || atomic_load(&idle) || !atomic_load(&ready)" in wake
assert "wake_diag: ready=%d allowed=%d hit=%u processed=%u drop=%u pcm_peak=%u wn_peak=%u clip=%u" in wake
assert "queued=%u fed=%u processed=%u drop=%u rebuild=%u hit=%u debounce=%u" in wake
assert "CONFIG_VOX_WAKE_GAIN=1" in sdk and "CONFIG_VOX_WAKE_THRESHOLD_PERCENT=60" in sdk
assert "CONFIG_VOX_ACOUSTIC_BARGE_IN=y" in sdk.splitlines()
report = turn.split("void app_turn_report(", 1)[1]
assert "结束: %s/%s" in report and "首I2S=%s" in report and "补缓冲=%u次/%.2fs" in report
failure = report.split("if (t->state == TURN_FAILED)", 1)[1].split("#if CONFIG_VOX_LOG_VERBOSE", 1)[0]
assert "ESP_LOGE" in failure and "heap_caps_get_largest_free_block" in failure
assert 'ESP_LOGE(TAG, "[%s] 阶段=%s，错误=%s"' in main
assert "结束: 继续聆听超时，无新语音" in main
assert "ASR failed: stage=%s" in asr and "TTS failed: stage=%s" in tts
assert "PCM underrun:" in tts and "PCM resumed:" in tts
assert tts.count("turn->tts_starvations == 1") == tts.count("turn->tts_starvations % 5 == 0") == 2
assert "if (stopped(stream))\n      APP_LOG_DETAIL" in tts
assert "else if (app_cloud_is_cancelled())\n    APP_LOG_DETAIL" in asr
assert "AQ connect: IPv4, socket_errno=%d, tls=%s" in read("app_network.c")
assert "WSS错误：握手HTTP=%d，TLS=0x%x" in tts
assert 'APP_LOG_DETAIL(TAG, "显示语音状态:' in read("app_ui.c")
assert 'APP_LOG_DETAIL(TAG, "上下文快照已保存' in read("app_storage.c")
print('PASS: serial-log policy, UTF-8/escaping models, errors and full SD IDs; scope=offline checks; pending=C/hardware validation')
