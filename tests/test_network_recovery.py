"""Local network recovery source guards and policy models. Compiler, serial, NTP and weather-API execution run separately."""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
network = (ROOT / "firmware/main/app_network.c").read_text()
header = (ROOT / "firmware/main/app_network.h").read_text()
main = (ROOT / "firmware/main/main.c").read_text()

init = network.split("esp_err_t app_network_init(void)", 1)[1].split(
    "bool app_network_is_connected(void)", 1)[0]
callback = network.split("static void wifi_event_handler", 1)[1].split(
    "esp_err_t app_network_init(void)", 1)[0]
maintenance = network.split("static void weather_update_task", 1)[1].split(
    "esp_err_t app_network_start_weather_updates(void)", 1)[0]
start = network.split("esp_err_t app_network_start_weather_updates(void)", 1)[1].split(
    "esp_err_t app_network_sync_time(void)", 1)[0]
request_sync = network.split("esp_err_t app_network_sync_time(void)", 1)[1].split(
    "static esp_err_t play_stream_pcm_bytes", 1)[0]
fetch = network.split("esp_err_t app_network_fetch_weather(void)", 1)[1].split(
    "static void weather_update_task", 1)[0]

# Local startup proceeds independently of access-point and time-server readiness.
assert "xEventGroupWaitBits" not in init and "portMAX_DELAY" not in init
assert "esp_netif_sntp" not in init
assert "app_network_sync_time()" not in main
assert "app_network_start_weather_updates()" in main

# Event callbacks cannot race SNTP initialization/deinitialization or retry forever
# inside the event task. Exactly one maintenance worker owns those operations.
assert "esp_wifi_connect" not in callback
assert "esp_netif_sntp" not in callback
assert "WIFI_MAX_RETRY" not in network
assert "atomic_compare_exchange_strong(&s_maintenance_started" in start
assert "atomic_store(&s_maintenance_started, false)" in start
assert network.count("xTaskCreate(weather_update_task") == 1
assert "#define WEATHER_TASK_STACK_SIZE 8192" in network
assert 'xTaskCreate(reminder_task, "reminder", 8192, NULL, 3, NULL)' in main
assert "联网、校时与天气维护任务已启动" in start
assert "本地提醒任务已启动" in main
assert "启动内存：内部空闲=" in main
for source in (start, main):
    assert "heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)" in source
    assert "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)" in source
assert "xEventGroupSetBits(s_wifi_event_group, TIME_SYNC_BIT)" in request_sync
assert "esp_netif_sntp" not in request_sync

# Each retry is bounded, but failed initial attempts never disable future retries.
assert "while (true)" in maintenance
assert "connecting && now >= wifi_deadline" in maintenance
assert "!connecting && now >= wifi_due" in maintenance
assert maintenance.count("wifi_due = now + wifi_retry_delay(&wifi_failures)") == 3
backoff = network.split("static int64_t wifi_retry_delay", 1)[1].split(
    "static void weather_update_task", 1)[0]
assert "2U << (*failures)++" in backoff
assert "WIFI_RETRY_INTERVAL_US / 1000000U" in backoff
assert "wifi_failures = 0;" in maintenance
assert "reason=%u" in callback
assert "pdMS_TO_TICKS(1000)" in maintenance
assert "vTaskDelay(next_delay)" not in maintenance

# Three independent server names are tried using the existing single NTP slot.
assert all(name in maintenance for name in (
    "ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org"))
assert "ESP_NETIF_SNTP_DEFAULT_CONFIG(time_servers[server])" in maintenance
assert "esp_netif_sntp_sync_wait(0)" in maintenance
assert "now >= time_deadline" in maintenance
assert "time_due = now + TIME_RETRY_INTERVAL_US" in maintenance
assert "server = (server + 1) %" in maintenance
assert "esp_netif_sntp_deinit()" in maintenance

# Reconnection and a newly valid time both bring the next weather fetch forward;
# the old ten-minute refresh delay cannot keep the first result waiting.
assert "connected && (!was_connected || (events & NETWORK_CHANGED_BIT))" in maintenance
assert "time_due = weather_due = air_due = 0" in maintenance
assert "if (weather_error != ESP_OK) weather_due = 0;" in maintenance
assert "weather_error = app_network_fetch_weather()" in maintenance
assert "WEATHER_REFRESH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS" in maintenance
assert "AIR_REFRESH_INTERVAL_MS : AIR_RETRY_INTERVAL_MS" in maintenance
assert "now >= air_due" in maintenance
assert "fetch_air_quality()" not in fetch
assert maintenance.index("weather_error = app_network_fetch_weather()") < maintenance.index("air_error = fetch_air_quality()")
boot = main.split("void app_main(void)", 1)[1]
assert boot.index("app_network_init()") < boot.index("app_network_start_weather_updates()") < boot.index("app_wake_init(handle_wake)")

# No fake wall clock, weakened TLS or background LVGL lock wait.
assert "time(NULL) < 1704067200" in fetch
assert ".crt_bundle_attach = esp_crt_bundle_attach" in fetch
assert "app_ui_set_weather" not in fetch
assert not re.search(r"\b(settimeofday|clock_settime|nvs_flash_erase)\s*\(", network)
assert "skip_cert_common_name_check" not in network
assert "app_network_status_snapshot" in header
assert all(field in header for field in (
    "time_valid", "time_syncing", "weather_updating", "weather_error",
    "time_retry_seconds", "weather_retry_seconds"))

# Catch malformed edits without trying to compile firmware.
token = re.compile(r'"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/', re.S)
for source in (network, header, main):
    stack = []
    for char in token.sub("", source):
        if char in "({[":
            stack.append(char)
        elif char in ")}]":
            assert stack and stack.pop() == {")": "(", "}": "{", "]": "["}[char]
    assert not stack

print('PASS: network recovery source guards; scope=offline checks; pending=C build, API and hardware validation')
