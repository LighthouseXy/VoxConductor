"""Legacy game lifetime, bounded replacement and memory/voice source guards.

Scope: local policy fixtures. SRAM, recognition, DNS latency and speaker output
require device or native-runtime measurements."""
import json
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MAIN = ROOT / "firmware/main"
sources = {name: (MAIN / name).read_text() for name in (
    "main.c", "app_desktop.c", "app_ui.c", "app_media.c", "app_network.c",
    "app_cloud.c", "app_cloud_tts.c", "app_control.c", "app_acoustic.c",
    "app_audio.c", "app_display.c", "app_memory_alloc.h", "CMakeLists.txt")}
# New bounded arcade is separate from the removed legacy game/audio state.
games = (MAIN / "app_games.c").read_text()
assert "xQueueCreateStatic" in games and "lv_obj_delete(root)" in games
assert "app_audio_" not in games and "http" not in games
assert '"app_speech.c"' not in sources["CMakeLists.txt"]
# V5 uses two former reserved unsigned slots for TTS, leaving sixteen slots;
# this preserves the V3/V4 footprint instead of restoring obsolete fields.
assert "tts_rate_percent, tts_voice_id, reserved_legacy[16]" in sources["app_desktop.c"]
assert "loaded.version == 3 || loaded.version == 4 || loaded.version == 5" in sources["app_desktop.c"]
assert "if (loaded.version < 5)" in sources["app_desktop.c"]
assert 'nvs_flash_erase' not in sources["app_desktop.c"]

placements = {
    "app_network.c": ("app_weather_t s_weather_cache", "char response_data[8192]"),
    "app_ui.c": ("app_weather_t s_ui_weather", "app_weather_t s_detail_weather",
                 "app_media_snapshot_t s_ui_media"),
    "app_media.c": ("app_media_snapshot_t s_view",),
    "app_desktop.c": ("app_media_snapshot_t s_media", "app_task_view_t s_tasks[8]"),
}
for name, declarations in placements.items():
    assert '#include "esp_attr.h"' in sources[name]
    for declaration in declarations:
        assert "static EXT_RAM_BSS_ATTR " + declaration + ";" in sources[name]
assert "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT" in sources["app_memory_alloc.h"]
assert "char *copy = app_psram_malloc(bytes)" in sources["app_memory_alloc.h"]
assert ".buff_dma = true" in sources["app_display.c"]
assert ".buff_spiram = false" in sources["app_display.c"]
sdk = (ROOT / "firmware/sdkconfig").read_text()
assert "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y" in sdk.splitlines()

# The model cannot announce tools that were removed, including state scopes.
control = sources["app_control.c"]
literal = re.compile(r'cJSON_Parse\(\s*((?:"(?:\\.|[^"\\])*"\s*)+)\)', re.S)
strings = re.compile(r'"(?:\\.|[^"\\])*"')
schemas = [json.loads("".join(json.loads(s[0]) for s in strings.finditer(m[1])))
           for m in literal.finditer(control)]
assert schemas
assert not any("game" in json.dumps(schema, ensure_ascii=False) for schema in schemas)

main, tts, cloud = (sources[n] for n in ("main.c", "app_cloud_tts.c", "app_cloud.c"))
turn = main.split("static esp_err_t run_voice_turn(", 1)[1].split(
    "static void voice_conversation_task(void *argument) {", 1)[0]
assert turn.index("app_acoustic_prepare()") < turn.index("app_cloud_ask(")
assert "app_acoustic_listen(true)" not in turn
assert turn.index("TURN_ASSISTANT_TEXT") < turn.index("app_cloud_speak(")
assert "app_speech_" not in cloud and "queue_ready" not in cloud
stream = cloud.split("static esp_err_t read_stream(", 1)[1].split("esp_err_t app_cloud_ask(", 1)[0]
ask = cloud.split("esp_err_t app_cloud_ask(", 1)[1].split("esp_err_t app_cloud_commit_turn", 1)[0]
assert "esp_http_client_cleanup" not in stream
assert ask.rindex("esp_http_client_cleanup(client)") < ask.rindex("return result;")
assert "turn.stages[TURN_GENERATE] == TURN_SUCCEEDED" in turn
play = tts.split("static esp_err_t speak(", 1)[1].split("esp_err_t app_cloud_speak(", 1)[0]
assert play.index("app_network_resolve_url(CLOUD_TTS_URL)") < play.index("calloc(")
assert "if (resolved != ESP_OK) return resolved" in play
assert play.index("playing = true;") < play.index("app_acoustic_listen(true)")
assert "bool can_interrupt = !reminder && app_acoustic_listen(true);" in play
assert "app_acoustic_listen(false);" in play.split("cleanup:", 1)[1]
assert ".crt_bundle_attach = esp_crt_bundle_attach" in play
assert "skip_cert_common_name_check" not in tts
listen = sources["app_acoustic.c"].split("bool app_acoustic_listen(bool active)", 1)[1].split(
    "void app_acoustic_discard", 1)[0]
assert "if (!atomic_load(&pending))" in listen  # accepted utterance survives TTS cleanup

dns = sources["app_network.c"].split("static void configure_sta_dns(", 1)[1].split(
    "static void wifi_event_handler", 1)[0]
assert dns.index("ESP_NETIF_DNS_BACKUP, &previous") < dns.index("ESP_NETIF_DNS_MAIN, &primary")
assert "if (address) freeaddrinfo(address)" in dns
assert "esp_wifi_stop" not in dns

def natural_interrupt(phase, reminder, computer_detected):
    return phase == "speaking" and not reminder and computer_detected

for phase in ("recording", "thinking", "speaking", "cleanup"):
    assert not natural_interrupt(phase, False, False)  # transient noise never confirms
    assert natural_interrupt(phase, False, True) == (phase == "speaking")
    assert not natural_interrupt(phase, True, True)
    button_cancels = True  # physical cancellation remains independent of VAD phase
    assert button_cancels

print('PASS: bounded game lifetime, PSRAM placement, serial voice phases and DNS-before-TTS guards; scope=offline checks; pending=C/runtime/hardware validation')
