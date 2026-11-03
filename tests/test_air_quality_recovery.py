"""Air-quality cache source guards and local Python state fixtures. Native cache and device acceptance run separately."""
from pathlib import Path
import json

ROOT = Path(__file__).resolve().parents[1]
source = (ROOT / "firmware/main/app_network.c").read_text()
header = (ROOT / "firmware/main/app_weather.h").read_text()
ui = (ROOT / "firmware/main/app_ui.c").read_text()
save = source.split("static void save_air_quality_cache(", 1)[1].split(
    "static void restore_air_quality_cache(", 1)[0]
restore = source.split("static void restore_air_quality_cache(", 1)[1].split(
    "static esp_err_t fetch_air_quality(", 1)[0]
fetch = source.split("static esp_err_t fetch_air_quality(", 1)[1].split(
    "esp_err_t app_network_fetch_weather(", 1)[0]
maintenance = source.split("static void weather_update_task(", 1)[1].split(
    "esp_err_t app_network_start_weather_updates(", 1)[0]
dns = source.split("static void configure_sta_dns(", 1)[1].split(
    "static void wifi_event_handler(", 1)[0]
got_ip = source.split("if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)", 1)[1].split(
    "esp_err_t app_network_init(", 1)[0]

assert 'air-quality-api.open-meteo.com' in header and '&current=us_aqi,pm2_5' in header
assert '.addr_type = HTTP_ADDR_TYPE_INET' in fetch
assert '.crt_bundle_attach = esp_crt_bundle_attach' in fetch
assert 'skip_cert_common_name_check' not in source
assert 'ESP_IP4ADDR_INIT(223, 5, 5, 5)' in dns
assert 'esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &primary)' in dns
assert 'ESP_NETIF_DNS_BACKUP, &previous' in dns
assert dns.index('ESP_NETIF_DNS_BACKUP, &previous') < dns.index('ESP_NETIF_DNS_MAIN, &primary')
assert 'esp_wifi_stop' not in dns
assert got_ip.index('configure_sta_dns(event->esp_netif)') < got_ip.index('xEventGroupSetBits')
assert 'getaddrinfo(APP_AIR_QUALITY_HOST, "443", &hints, &address)' in fetch
assert 'if (address) freeaddrinfo(address)' in fetch
assert fetch.index('AQ DNS: %s -> %s') < fetch.index('esp_http_client_perform(client)')
assert '152.53.84.73' not in source and '103.73.220.188' not in source
assert 'APP_AIR_QUALITY_HOST "/v1/air-quality' in header
for api in ('esp_http_client_get_errno(client)',
            'esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags)'):
    assert fetch.index(api) < fetch.index('esp_http_client_cleanup(client)')
assert 'socket_errno=%d' in fetch and 'largest=%u B' in fetch
assert 'if (changed) save_air_quality_cache()' in fetch
assert fetch.index('portEXIT_CRITICAL(&s_weather_lock)') < fetch.index('save_air_quality_cache()')
assert 'nvs_set_str(handle, "air", response_data)' in save
assert 'nvs_commit(handle)' in save and 'nvs_close(handle)' in save
assert 'app_weather_parse_air(response_data, &air)' in restore
assert 's_weather_cache.air_cached = true' in restore
assert 's_weather_cached_us =' not in restore
assert maintenance.index('restore_air_quality_cache()') < maintenance.index('while (true)')
assert 'AIR_REFRESH_INTERVAL_MS : AIR_RETRY_INTERVAL_MS' in maintenance
assert 'weather.air = s_weather_cache.air' in source
assert 's_weather_cache.air_cached = s_weather_cache.air.available' in fetch
assert 'same_day = air->available' in ui and 'US AQI 缓存' in ui

# Python state fixtures cover cache semantics; C/NVS execution is a separate check.
payload = {
    'current_units': {'us_aqi': 'USAQI', 'pm2_5': 'μg/m³'},
    'current': {'time': '2026-10-06T20:00', 'us_aqi': 94, 'pm2_5': 107.6},
}
class Cache:
    def __init__(self, flash=None):
        self.flash = flash or {}
        self.air, self.cached, self.writes = None, False, 0
        self.forecast_valid = False
        if 'air' in self.flash:
            try:
                data = json.loads(self.flash['air'])
                units, air = data['current_units'], data['current']
                if (units == payload['current_units'] and 0 <= air['us_aqi'] <= 1000
                        and 0 <= air['pm2_5'] <= 10000 and len(air['time']) == 16):
                    self.air, self.cached = air, True
            except (ValueError, TypeError, KeyError):
                pass
    def update(self, data=None):
        if data is None:
            self.cached = self.air is not None
            return
        air = data['current']
        if self.air != air:
            self.flash['air'] = json.dumps(data)
            self.writes += 1
        self.air, self.cached = air.copy(), False

first = Cache()
first.update()  # first boot + connection timeout keep AQI unavailable
assert first.air is None and not first.cached and first.writes == 0
first.update(payload)
assert first.air['us_aqi'] == 94 and not first.cached and first.writes == 1
first.update(payload)  # unchanged hourly observation preserves the flash write count
assert first.writes == 1
reboot = Cache(first.flash)
assert reboot.cached and reboot.air['us_aqi'] == 94 and not reboot.forecast_valid
reboot.update()  # network failure preserves a visibly cached value
assert reboot.cached and reboot.air['us_aqi'] == 94 and reboot.writes == 0
assert reboot.air['time'][:10] == '2026-10-06'
assert reboot.air['time'][:10] != '2026-10-07'  # existing UI hides a prior day's AQI
reboot.update(payload)
assert not reboot.cached and reboot.writes == 0
assert Cache({'air': 'truncated JSON'}).air is None
bad = json.loads(json.dumps(payload))
bad['current_units']['us_aqi'] = 'China AQI'
assert Cache({'air': json.dumps(bad)}).air is None
print('PASS: AQ IPv4/TLS diagnostics, cache restore/save/failure/reboot fixtures; scope=offline checks; pending=C/API/device validation')
