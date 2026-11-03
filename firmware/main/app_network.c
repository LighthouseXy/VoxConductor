/*
 * 网络维护：Wi-Fi重连、校时、天气/空气质量更新和只读快照。
 * 失败保留上次有效值并退避重试，不伪装成刚更新的数据。
 * 语音优先时减少后台联网竞争，事件回调不执行长时间HTTP。
 */
#include "esp_attr.h"
#include "app_network.h"
#include "app_log.h"

#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "app_audio.h"
#include "app_storage.h"
#include "app_ui.h"
#include "app_weather.h"
#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_ip_addr.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "lwip/inet.h"
#include "lwip/netdb.h"
#include "wifi_credentials.h"

#define WIFI_CONNECTED_BIT BIT0
#define NETWORK_CHANGED_BIT BIT1
#define WIFI_DISCONNECTED_BIT BIT2
#define TIME_SYNC_BIT BIT3
#define WIFI_CONNECT_TIMEOUT_US 25000000
#define WIFI_RETRY_INTERVAL_US 30000000
#define TIME_SYNC_TIMEOUT_US 30000000
#define TIME_RETRY_INTERVAL_US 15000000

// 旧服务端语音接口仅保留为迁移参考；当前按键与天气均直接访问云端。
#define AUDIO_TURN_URL "http://192.168.1.16:8000/audio/turn"
#define AUDIO_TURN_METADATA_URL_PREFIX "http://192.168.1.16:8000/audio/turn/"

#define DEVICE_SESSION_ID "voxconductor-01"
#define TURN_METADATA_BUFFER_SIZE 4096
#define TTS_PLAYBACK_FRAMES_PER_BLOCK 256
// 约340ms音频，用来吸收HTTP和Wi-Fi短时抖动。
#define TTS_STREAM_PREBUFFER_BYTES (16 * 1024)
// 获取失败后30秒重试；成功后每10分钟更新一次。
#define WEATHER_RETRY_INTERVAL_MS 30000
#define WEATHER_REFRESH_INTERVAL_MS (10 * 60 * 1000)
#define AIR_RETRY_INTERVAL_MS 30000
#define AIR_REFRESH_INTERVAL_MS (10 * 60 * 1000)
#define WEATHER_TASK_STACK_SIZE 8192

static const char *TAG = "app_network";
static portMUX_TYPE s_weather_lock = portMUX_INITIALIZER_UNLOCKED;
static EXT_RAM_BSS_ATTR app_weather_t s_weather_cache;
static int64_t s_weather_cached_us;
static app_network_status_t s_network_status;
static int64_t s_time_retry_us, s_weather_retry_us;
static EventGroupHandle_t s_wifi_event_group;
static bool s_network_started;
static _Atomic bool s_maintenance_started;
static _Atomic bool s_voice_priority;
void app_network_voice_priority(bool active) { atomic_store(&s_voice_priority, active); }
// Forecast and air quality run serially in the same maintenance owner.
static EXT_RAM_BSS_ATTR char response_data[8192];

void app_network_status_snapshot(app_network_status_t *out) {
  if (!out) return;
  portENTER_CRITICAL(&s_weather_lock);
  *out = s_network_status;
  int64_t time_due = s_time_retry_us, weather_due = s_weather_retry_us;
  portEXIT_CRITICAL(&s_weather_lock);
  int64_t now = esp_timer_get_time();
  out->connected = app_network_is_connected();
  out->time_valid = time(NULL) >= 1704067200;
  out->time_retry_seconds = time_due > now ? (unsigned)((time_due - now + 999999) / 1000000) : 0;
  out->weather_retry_seconds = weather_due > now ? (unsigned)((weather_due - now + 999999) / 1000000) : 0;
}

static void network_progress(bool syncing, bool updating, esp_err_t weather_error,
                             int64_t time_due, int64_t weather_due) {
  portENTER_CRITICAL(&s_weather_lock);
  s_network_status.time_syncing = syncing;
  s_network_status.weather_updating = updating;
  s_network_status.weather_error = weather_error;
  s_time_retry_us = time_due;
  s_weather_retry_us = weather_due;
  portEXIT_CRITICAL(&s_weather_lock);
}

bool app_network_weather_snapshot(app_weather_t *out, unsigned *age_seconds) {
  if (!out || !age_seconds) return false;
  portENTER_CRITICAL(&s_weather_lock);
  *out = s_weather_cache;
  int64_t cached = s_weather_cached_us;
  portEXIT_CRITICAL(&s_weather_lock);
  *age_seconds = cached ? (unsigned)((esp_timer_get_time() - cached) / 1000000) : 0;
  return cached != 0;
}
static uint8_t s_tts_stream_prebuffer[TTS_STREAM_PREBUFFER_BYTES];

typedef struct {
  char *data;
  size_t capacity;
  size_t length;
  bool overflow;
} http_response_buffer_t;

typedef struct {
  const char *expected_turn_id;
  bool turn_id_verified;
  size_t total_bytes;
  size_t prebuffer_length;
  uint8_t pending_byte;
  bool has_pending_byte;
  bool playback_started;
  esp_err_t playback_result;
} streaming_audio_context_t;

static esp_err_t init_nvs(void) {
  // 网络初始化不能为了恢复自身而擦除桌面设置。
  return nvs_flash_init();
}

static void configure_sta_dns(esp_netif_t *netif) {
  // Keep DHCP's primary as a real backup before replacing the primary resolver.
  esp_netif_dns_info_t previous = {0};
  if (esp_netif_get_dns_info(netif, ESP_NETIF_DNS_MAIN, &previous) == ESP_OK &&
      previous.ip.type == ESP_IPADDR_TYPE_V4)
    ESP_LOGI(TAG, "DHCP DNS: " IPSTR, IP2STR(&previous.ip.u_addr.ip4));
  esp_netif_dns_info_t primary = {.ip = ESP_IP4ADDR_INIT(223, 5, 5, 5)};
  if (previous.ip.type == ESP_IPADDR_TYPE_V4 && previous.ip.u_addr.ip4.addr &&
      previous.ip.u_addr.ip4.addr != primary.ip.u_addr.ip4.addr) {
    esp_err_t backup = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_BACKUP, &previous);
    if (backup != ESP_OK)
      ESP_LOGW(TAG, "DHCP DNS backup failed: %s", esp_err_to_name(backup));
  }
  esp_err_t result = esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &primary);
  if (result == ESP_OK)
    ESP_LOGI(TAG, "STA DNS: " IPSTR " (DHCP resolver retained as backup)", IP2STR(&primary.ip.u_addr.ip4));
  else
    ESP_LOGW(TAG, "STA DNS configuration failed: %s", esp_err_to_name(result));
}

esp_err_t app_network_resolve_url(const char *url) {
  if (!url) return ESP_ERR_INVALID_ARG;
  const char *host = strstr(url, "://");
  if (!host) return ESP_ERR_INVALID_ARG;
  host += 3;
  size_t bytes = strcspn(host, ":/?#");
  char name[128];
  if (!bytes || bytes >= sizeof(name) || memchr(host, '@', bytes))
    return ESP_ERR_INVALID_ARG;
  if (!app_network_is_connected()) return ESP_ERR_INVALID_STATE;
  memcpy(name, host, bytes);
  name[bytes] = 0;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *address = NULL;
  int error = getaddrinfo(name, NULL, &hints, &address);
  bool resolved = error == 0 && address != NULL;
  if (address) freeaddrinfo(address);
  if (resolved) return ESP_OK;
  ESP_LOGW(TAG, "Cloud DNS failed: host=%s, code=%d, internal=%u B, largest=%u B",
      name, error,
      (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
      (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  return error == EAI_MEMORY ? ESP_ERR_NO_MEM : ESP_ERR_NOT_FOUND;
}

static void wifi_event_handler(void *argument, esp_event_base_t event_base,
                               int32_t event_id, void *event_data) {
  (void)argument;

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    xEventGroupSetBits(s_wifi_event_group, NETWORK_CHANGED_BIT);
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    const wifi_event_sta_disconnected_t *event = event_data;
    ESP_LOGW(TAG, "Wi-Fi disconnected: reason=%u", event ? (unsigned)event->reason : 0U);
    xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    app_ui_set_wifi_connected(false);

    // 事件回调不重连、不校时；唯一维护任务负责有界尝试和后续重试。
    xEventGroupSetBits(s_wifi_event_group, WIFI_DISCONNECTED_BIT | NETWORK_CHANGED_BIT);
    return;
  }

  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)event_data;
    ESP_LOGI(TAG, "Wi-Fi连接成功，IP地址：" IPSTR, IP2STR(&event->ip_info.ip));
    // Apply after DHCP, before waking tasks that may start their first DNS query.
    configure_sta_dns(event->esp_netif);
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT | NETWORK_CHANGED_BIT);
    app_ui_set_wifi_connected(true);
  }
}

esp_err_t app_network_init(void) {
  if (s_network_started) return ESP_OK;
  setenv("TZ", "CST-8", 1);
  tzset();
  esp_err_t result = init_nvs();
  if (result != ESP_OK) {
    return result;
  }

  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == NULL) {
    return ESP_ERR_NO_MEM;
  }

  if ((result = esp_netif_init()) != ESP_OK ||
      (result = esp_event_loop_create_default()) != ESP_OK) {
    return result;
  }
  if (esp_netif_create_default_wifi_sta() == NULL) {
    return ESP_FAIL;
  }

  const wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
  if ((result = esp_wifi_init(&init_config)) != ESP_OK ||
      (result = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                           wifi_event_handler, NULL)) != ESP_OK ||
      (result = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                           wifi_event_handler, NULL)) != ESP_OK) {
    return result;
  }

  wifi_config_t wifi_config = {
      .sta =
          {
              .ssid = WIFI_SSID,
              .password = WIFI_PASSWORD,
              .threshold.authmode = WIFI_AUTH_WPA2_PSK,
              .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
          },
  };

  if ((result = esp_wifi_set_mode(WIFI_MODE_STA)) != ESP_OK ||
      (result = esp_wifi_set_config(WIFI_IF_STA, &wifi_config)) != ESP_OK ||
      (result = esp_wifi_start()) != ESP_OK) {
    return result;
  }

  s_network_started = true;
  ESP_LOGI(TAG, "Wi-Fi已启动，后台连接与校时不阻塞本地功能");
  return ESP_OK;
}

bool app_network_is_connected(void) {
  return s_wifi_event_group != NULL &&
         (xEventGroupGetBits(s_wifi_event_group) & WIFI_CONNECTED_BIT) != 0;
}

static esp_err_t text_http_event_handler(esp_http_client_event_t *event) {
  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
      event->user_data == NULL) {
    return ESP_OK;
  }

  http_response_buffer_t *response = (http_response_buffer_t *)event->user_data;
  const size_t available = response->capacity - response->length - 1;
  size_t copy_length = (size_t)event->data_len;
  if (copy_length > available) {
    copy_length = available;
  }
  if (copy_length > 0) {
    memcpy(response->data + response->length, event->data, copy_length);
    response->length += copy_length;
    response->data[response->length] = '\0';
  }
  if (copy_length < (size_t)event->data_len) {
    response->overflow = true;
    ESP_LOGW(TAG, "HTTP文本响应超过缓冲区，后面的内容已截断");
  }
  return ESP_OK;
}

// The maintenance owner alone accesses this small, validated JSON cache.
// Persist hourly data when the observation changes.
static void save_air_quality_cache(void) {
  nvs_handle_t handle;
  esp_err_t result = nvs_open("weather", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result = nvs_set_str(handle, "air", response_data);
    if (result == ESP_OK) result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK)
    ESP_LOGW(TAG, "AQ cache save failed: %s", esp_err_to_name(result));
}

static void restore_air_quality_cache(void) {
  nvs_handle_t handle;
  if (nvs_open("weather", NVS_READONLY, &handle) != ESP_OK) return;
  size_t bytes = sizeof(response_data);
  esp_err_t result = nvs_get_str(handle, "air", response_data, &bytes);
  nvs_close(handle);
  app_air_quality_t air = {0};
  if (result != ESP_OK || !app_weather_parse_air(response_data, &air)) return;
  portENTER_CRITICAL(&s_weather_lock);
  s_weather_cache.air = air;
  s_weather_cache.air_cached = true;
  portEXIT_CRITICAL(&s_weather_lock);
  // A restored AQ value never marks the forecast or wall clock as valid.
  ESP_LOGI(TAG, "AQ cache restored: US AQI=%d, time=%s (cached observation)",
           air.us_aqi, air.updated_at);
}

// Independent AQ retry schedule, shared worker/buffer, unchanged verified TLS.
static esp_err_t fetch_air_quality(void) {
  if (!app_network_is_connected() || time(NULL) < 1704067200)
    return ESP_ERR_INVALID_STATE;
  struct addrinfo hints = {.ai_family = AF_INET, .ai_socktype = SOCK_STREAM};
  struct addrinfo *address = NULL;
  int dns_result = getaddrinfo(APP_AIR_QUALITY_HOST, "443", &hints, &address);
  if (dns_result == 0 && address) {
    char ip[INET_ADDRSTRLEN];
    const struct sockaddr_in *v4 = (const struct sockaddr_in *)address->ai_addr;
    if (inet_ntop(AF_INET, &v4->sin_addr, ip, sizeof(ip)))
      APP_LOG_DETAIL(TAG, "AQ DNS: %s -> %s", APP_AIR_QUALITY_HOST, ip);
  } else {
    ESP_LOGW(TAG, "AQ DNS lookup failed: %d", dns_result);
  }
  if (address) freeaddrinfo(address);
  memset(response_data, 0, sizeof(response_data));
  http_response_buffer_t response = {
      .data = response_data, .capacity = sizeof(response_data)};
  const esp_http_client_config_t config = {
      .url = APP_AIR_QUALITY_URL, .method = HTTP_METHOD_GET, .timeout_ms = 15000,
      // Select routable IPv4 DNS results for this STA.
      .addr_type = HTTP_ADDR_TYPE_INET,
      .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
      .user_agent = "VoxConductor/1.0", .event_handler = text_http_event_handler,
      .user_data = &response};
  esp_http_client_handle_t client = esp_http_client_init(&config);
  esp_err_t result = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
  int status = client ? esp_http_client_get_status_code(client) : 0;
  int socket_error = 0, tls_code = 0, tls_flags = 0;
  esp_err_t tls_error = ESP_OK;
  if (client && result != ESP_OK) {
    socket_error = esp_http_client_get_errno(client);
    tls_error = esp_http_client_get_and_clear_last_tls_error(client, &tls_code, &tls_flags);
  }
  if (client) esp_http_client_cleanup(client);
  app_air_quality_t air = {0};
  if (result == ESP_OK &&
      (status != 200 || response.overflow || !app_weather_parse_air(response_data, &air)))
    result = ESP_ERR_INVALID_RESPONSE;
  bool changed = false, have_cache;
  portENTER_CRITICAL(&s_weather_lock);
  if (result == ESP_OK) {
    changed = !s_weather_cache.air.available ||
        strcmp(s_weather_cache.air.updated_at, air.updated_at) ||
        s_weather_cache.air.us_aqi != air.us_aqi || s_weather_cache.air.pm25 != air.pm25;
    s_weather_cache.air = air;
    s_weather_cache.air_cached = false;
  } else {
    s_weather_cache.air_cached = s_weather_cache.air.available;
  }
  have_cache = s_weather_cache.air.available;
  portEXIT_CRITICAL(&s_weather_lock);
  if (changed) save_air_quality_cache(); // Never write flash under the cache lock.
  if (result == ESP_OK)
    ESP_LOGI(TAG, "Air quality updated: US AQI=%d, PM2.5=%.1f ug/m3, time=%s",
             air.us_aqi, (double)air.pm25, air.updated_at);
  else {
    ESP_LOGW(TAG, "空气质量未更新：HTTP=%d，%s，30秒后重试，%s", status,
             esp_err_to_name(result), have_cache ? "保留上次数据" : "尚无成功数据");
    ESP_LOGW(TAG, "AQ connect: IPv4, socket_errno=%d, tls=%s, code=%d, flags=0x%x, cached=%d, internal=%u B, largest=%u B",
             socket_error, esp_err_to_name(tls_error), tls_code, (unsigned)tls_flags,
             (int)have_cache,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
  }
  return result;
}

esp_err_t app_network_fetch_weather(void) {
  if (!app_network_is_connected()) return ESP_ERR_INVALID_STATE;
  if (time(NULL) < 1704067200) {
    ESP_LOGW(TAG, "天气等待校时完成，暂不发起HTTPS请求");
    return ESP_ERR_INVALID_STATE;
  }
  memset(response_data, 0, sizeof(response_data));

  http_response_buffer_t response = {
      .data = response_data,
      .capacity = sizeof(response_data),
  };

  const esp_http_client_config_t config = {
      .url = APP_WEATHER_URL,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 15000,
      .crt_bundle_attach = esp_crt_bundle_attach,
      .disable_auto_redirect = true,
      .user_agent = "VoxConductor/1.0",
      .event_handler = text_http_event_handler,
      .user_data = &response,
  };

  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }

  const esp_err_t request_result =
      esp_http_client_perform(client);
  const int status_code =
      esp_http_client_get_status_code(client);

  esp_http_client_cleanup(client);

  if (request_result != ESP_OK || status_code != 200) {
    ESP_LOGW(TAG, "Open-Meteo请求失败：HTTP=%d，%s", status_code,
             esp_err_to_name(request_result));
    return request_result != ESP_OK
               ? request_result
               : ESP_FAIL;
  }

  app_weather_t weather;
  if (response.overflow || !app_weather_parse(response_data, &weather)) {
    ESP_LOGW(TAG, "Open-Meteo响应无效：%u字节，溢出=%d",
             (unsigned)response.length, response.overflow);
    return ESP_ERR_INVALID_RESPONSE;
  }

  portENTER_CRITICAL(&s_weather_lock);
  weather.air = s_weather_cache.air;
  weather.air_cached = s_weather_cache.air_cached;
  portEXIT_CRITICAL(&s_weather_lock);

  portENTER_CRITICAL(&s_weather_lock);
  s_weather_cache = weather;
  s_weather_cached_us = esp_timer_get_time();
  portEXIT_CRITICAL(&s_weather_lock);
  // LVGL的一秒timer读取快照；网络任务不等待界面锁，也不丢失最后更新。

  ESP_LOGI(TAG, "天气直连更新：%s，%d°C，体感%d°C，%s，数据时间=%s",
      APP_WEATHER_CITY, weather.temperature_c, weather.apparent_c,
      weather.condition, weather.updated_at);
  return ESP_OK;
}

static int64_t wifi_retry_delay(unsigned *failures) {
  // Retry transient first-connection failures promptly, then cap at 30 seconds.
  unsigned seconds = *failures < 4 ? 2U << (*failures)++
                                  : WIFI_RETRY_INTERVAL_US / 1000000U;
  ESP_LOGW(TAG, "Wi-Fi retry in %u s", seconds);
  return (int64_t)seconds * 1000000;
}

static void weather_update_task(void *argument)
{
  (void)argument;
  static const char *const time_servers[] = {
      "ntp.aliyun.com", "time.cloudflare.com", "pool.ntp.org"};
  unsigned server = 0;
  unsigned wifi_failures = 0;
  bool connecting = false, was_connected = false;
  bool sntp_initialized = false, syncing = false;
  int64_t wifi_due = 0, wifi_deadline = 0;
  int64_t time_due = 0, time_deadline = 0, weather_due = 0, air_due = 0;
  esp_err_t weather_error = ESP_OK;

  restore_air_quality_cache();

  // 唯一owner维护SNTP和Wi-Fi重试；事件回调仅唤醒，不与deinit/init并发。
  while (true) {
    EventBits_t events = xEventGroupWaitBits(s_wifi_event_group,
        NETWORK_CHANGED_BIT | WIFI_DISCONNECTED_BIT | TIME_SYNC_BIT,
        pdTRUE, pdFALSE, pdMS_TO_TICKS(1000));
    int64_t now = esp_timer_get_time();
    bool connected = app_network_is_connected();
    if (connected && (!was_connected || (events & NETWORK_CHANGED_BIT))) {
      connecting = false;
      wifi_failures = 0;
      // 新连接不继承十分钟睡眠；恢复后立即重新校时、获取当前天气。
      time_due = weather_due = air_due = 0;
      events |= TIME_SYNC_BIT;
    }
    if (!connected) {
      if (sntp_initialized) {
        esp_netif_sntp_deinit();
        sntp_initialized = syncing = false;
      }
      if ((events & WIFI_DISCONNECTED_BIT) && (connecting || was_connected)) {
        connecting = false;
        wifi_due = now + wifi_retry_delay(&wifi_failures);
      }
      if (connecting && now >= wifi_deadline) {
        connecting = false;
        wifi_due = now + wifi_retry_delay(&wifi_failures);
        (void)esp_wifi_disconnect();
        ESP_LOGW(TAG, "Wi-Fi connection timed out");
      }
      if (!connecting && now >= wifi_due) {
        esp_err_t result = esp_wifi_connect();
        connecting = result == ESP_OK;
        wifi_deadline = now + WIFI_CONNECT_TIMEOUT_US;
        if (!connecting) {
          wifi_due = now + wifi_retry_delay(&wifi_failures);
          ESP_LOGW(TAG, "Wi-Fi connect start failed: %s", esp_err_to_name(result));
        } else ESP_LOGI(TAG, "开始Wi-Fi连接尝试，最长等待25秒");
      }
    } else {
      if (events & TIME_SYNC_BIT) {
        if (sntp_initialized) esp_netif_sntp_deinit();
        sntp_initialized = syncing = false;
        time_due = 0;
      }
      if (!sntp_initialized && now >= time_due) {
        // 当前IDF配置只有一个NTP槽，轮转服务器无需扩大sdkconfig。
        const esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG(time_servers[server]);
        esp_err_t result = esp_netif_sntp_init(&config);
        if (result == ESP_OK) {
          sntp_initialized = syncing = true;
          time_deadline = now + TIME_SYNC_TIMEOUT_US;
          ESP_LOGI(TAG, "后台校时：%s，最长等待30秒", time_servers[server]);
        } else {
          server = (server + 1) % (sizeof(time_servers) / sizeof(time_servers[0]));
          time_due = now + TIME_RETRY_INTERVAL_US;
          ESP_LOGW(TAG, "校时服务启动失败，15秒后换服务器重试：%s", esp_err_to_name(result));
        }
      }
      if (syncing) {
        esp_err_t result = esp_netif_sntp_sync_wait(0); // 真正的非阻塞查询，不占用主线程。
        if (result == ESP_OK && time(NULL) >= 1704067200) {
          syncing = false;
          time_due = 0;
          // SNTP may have made the time valid during the preceding HTTP fetch.
          // Retain the successful forecast/AQ result until its next scheduled refresh.
          if (weather_error != ESP_OK) weather_due = 0;
          time_t local = time(NULL);
          struct tm local_time;
          char time_text[32];
          localtime_r(&local, &local_time);
          strftime(time_text, sizeof(time_text), "%Y-%m-%d %H:%M:%S", &local_time);
          ESP_LOGI(TAG, "当前北京时间：%s", time_text);
        } else if (now >= time_deadline) {
          esp_netif_sntp_deinit();
          sntp_initialized = syncing = false;
          server = (server + 1) % (sizeof(time_servers) / sizeof(time_servers[0]));
          time_due = now + TIME_RETRY_INTERVAL_US;
          ESP_LOGW(TAG, "校时未完成，15秒后换服务器重试");
        }
      }
      if (!atomic_load(&s_voice_priority) && time(NULL) >= 1704067200 && now >= weather_due) {
        network_progress(syncing, true, weather_error, time_due, 0);
        weather_error = app_network_fetch_weather();
        now = esp_timer_get_time();
        weather_due = now + (int64_t)(weather_error == ESP_OK
            ? WEATHER_REFRESH_INTERVAL_MS : WEATHER_RETRY_INTERVAL_MS) * 1000;
        if (weather_error != ESP_OK)
          ESP_LOGW(TAG, "天气获取失败，30秒后重试：%s", esp_err_to_name(weather_error));
        network_progress(syncing, false, weather_error, time_due, weather_due);
      }
      if (!atomic_load(&s_voice_priority) && app_network_is_connected() && time(NULL) >= 1704067200 && now >= air_due) {
        esp_err_t air_error = fetch_air_quality();
        now = esp_timer_get_time();
        air_due = now + (int64_t)(air_error == ESP_OK
            ? AIR_REFRESH_INTERVAL_MS : AIR_RETRY_INTERVAL_MS) * 1000;
      }
    }
    network_progress(syncing, false, weather_error,
                     connected ? time_due : 0,
                     connected && time(NULL) >= 1704067200 ? weather_due : 0);
    was_connected = connected;
  }
}

esp_err_t app_network_start_weather_updates(void)
{
  if (!s_network_started || !s_wifi_event_group) return ESP_ERR_INVALID_STATE;
  bool expected = false;
  if (!atomic_compare_exchange_strong(&s_maintenance_started, &expected, true)) return ESP_OK;
  const BaseType_t result =
      xTaskCreate(weather_update_task,
                  "weather_update",
                  WEATHER_TASK_STACK_SIZE,
                  NULL,
                  4,
                  NULL);

  if (result != pdPASS) {
    atomic_store(&s_maintenance_started, false);
    ESP_LOGE(TAG, "创建天气更新任务失败：stack=%u B，内部空闲=%u B，最大连续块=%u B",
             (unsigned)WEATHER_TASK_STACK_SIZE,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    return ESP_ERR_NO_MEM;
  }

  ESP_LOGI(TAG, "联网、校时与天气维护任务已启动");
  return ESP_OK;
}

esp_err_t app_network_sync_time(void) {
  if (!s_network_started || !atomic_load(&s_maintenance_started)) return ESP_ERR_INVALID_STATE;
  xEventGroupSetBits(s_wifi_event_group, TIME_SYNC_BIT);
  return ESP_OK;
}

static esp_err_t play_stream_pcm_bytes(streaming_audio_context_t *context,
                                       const uint8_t *data, size_t length) {
  int16_t sample_block[TTS_PLAYBACK_FRAMES_PER_BLOCK];
  uint8_t *block_bytes = (uint8_t *)sample_block;
  size_t offset = 0;

  while (offset < length || context->has_pending_byte) {
    size_t block_length = 0;
    if (context->has_pending_byte) {
      block_bytes[block_length++] = context->pending_byte;
      context->has_pending_byte = false;
    }
    size_t copy_length = length - offset;
    const size_t available = sizeof(sample_block) - block_length;
    if (copy_length > available) {
      copy_length = available;
    }
    if (copy_length > 0) {
      memcpy(block_bytes + block_length, data + offset, copy_length);
      block_length += copy_length;
      offset += copy_length;
    }
    if ((block_length % sizeof(int16_t)) != 0) {
      context->pending_byte = block_bytes[block_length - 1];
      context->has_pending_byte = true;
      --block_length;
    }
    if (block_length == 0) {
      break;
    }
    const esp_err_t result = app_audio_play_mono_pcm(
        sample_block, block_length / sizeof(int16_t));
    if (result != ESP_OK) {
      return result;
    }
  }
  return ESP_OK;
}

static esp_err_t streaming_http_event_handler(esp_http_client_event_t *event) {
  streaming_audio_context_t *context =
      (streaming_audio_context_t *)event->user_data;

  if (event->event_id == HTTP_EVENT_ON_HEADER && context != NULL &&
      event->header_key != NULL && event->header_value != NULL &&
      strcasecmp(event->header_key, "X-Turn-ID") == 0) {
    if (strcmp(event->header_value, context->expected_turn_id) != 0) {
      ESP_LOGE(TAG, "收到错误轮次：期望%s，实际%s", context->expected_turn_id,
               event->header_value);
      return ESP_ERR_INVALID_RESPONSE;
    }
    context->turn_id_verified = true;
    return ESP_OK;
  }

  if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 ||
      context == NULL) {
    return ESP_OK;
  }
  if (esp_http_client_get_status_code(event->client) != 200) {
    return ESP_OK;
  }
  if (!context->turn_id_verified) {
    return ESP_ERR_INVALID_RESPONSE;
  }

  const uint8_t *data = (const uint8_t *)event->data;
  size_t length = (size_t)event->data_len;
  context->total_bytes += length;

  if (!context->playback_started) {
    const size_t remaining =
        TTS_STREAM_PREBUFFER_BYTES - context->prebuffer_length;
    const size_t copy_length = length < remaining ? length : remaining;
    memcpy(s_tts_stream_prebuffer + context->prebuffer_length, data,
           copy_length);
    context->prebuffer_length += copy_length;
    data += copy_length;
    length -= copy_length;
    if (context->prebuffer_length < TTS_STREAM_PREBUFFER_BYTES) {
      return ESP_OK;
    }

    context->playback_started = true;
    app_ui_show_voice_state(APP_UI_VOICE_SPEAKING, "正在播放回答");
    context->playback_result = play_stream_pcm_bytes(
        context, s_tts_stream_prebuffer, context->prebuffer_length);
    context->prebuffer_length = 0;
    if (context->playback_result != ESP_OK) {
      return context->playback_result;
    }
  }

  if (length > 0) {
    context->playback_result = play_stream_pcm_bytes(context, data, length);
  }
  return context->playback_result;
}

esp_err_t app_network_upload_voice_turn(const int16_t *recording,
                                        size_t recording_bytes,
                                        const char *turn_id) {
  if (recording == NULL || recording_bytes == 0 || turn_id == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  streaming_audio_context_t stream = {
      .expected_turn_id = turn_id,
      .playback_result = ESP_OK,
  };
  const esp_http_client_config_t config = {
      .url = AUDIO_TURN_URL,
      .method = HTTP_METHOD_POST,
      .timeout_ms = 180000,
      .event_handler = streaming_http_event_handler,
      .user_data = &stream,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }

  esp_http_client_set_header(client, "Content-Type",
                             "application/octet-stream");
  esp_http_client_set_header(client, "X-Session-ID", DEVICE_SESSION_ID);
  esp_http_client_set_header(client, "X-Turn-ID", turn_id);
  esp_http_client_set_post_field(client, (const char *)recording,
                                 (int)recording_bytes);
  ESP_LOGI(TAG, "正在上传语音：session=%s，turn=%s，%u字节",
           DEVICE_SESSION_ID, turn_id, (unsigned)recording_bytes);

  const esp_err_t request_result = esp_http_client_perform(client);
  const int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (request_result != ESP_OK || status_code != 200) {
    return request_result != ESP_OK ? request_result : ESP_FAIL;
  }

  // 极短回答不足预缓冲大小时，在HTTP结束后播放剩余音频。
  if (!stream.playback_started && stream.prebuffer_length > 0) {
    stream.playback_started = true;
    app_ui_show_voice_state(APP_UI_VOICE_SPEAKING, "正在播放回答");
    stream.playback_result = play_stream_pcm_bytes(
        &stream, s_tts_stream_prebuffer, stream.prebuffer_length);
  }
  if (stream.playback_result != ESP_OK) {
    return stream.playback_result;
  }
  if (stream.total_bytes == 0 || stream.has_pending_byte) {
    return ESP_ERR_INVALID_SIZE;
  }
  ESP_LOGI(TAG, "AI流式播放完成：%u字节", (unsigned)stream.total_bytes);
  return ESP_OK;
}

esp_err_t app_network_fetch_and_store_turn(const char *turn_id) {
  if (turn_id == NULL) {
    return ESP_ERR_INVALID_ARG;
  }
  char turn_url[192];
  const int url_length = snprintf(turn_url, sizeof(turn_url), "%s%s",
                                  AUDIO_TURN_METADATA_URL_PREFIX, turn_id);
  if (url_length < 0 || (size_t)url_length >= sizeof(turn_url)) {
    return ESP_ERR_INVALID_SIZE;
  }

  static char response_data[TURN_METADATA_BUFFER_SIZE];
  memset(response_data, 0, sizeof(response_data));
  http_response_buffer_t response = {
      .data = response_data,
      .capacity = sizeof(response_data),
  };
  const esp_http_client_config_t config = {
      .url = turn_url,
      .method = HTTP_METHOD_GET,
      .timeout_ms = 15000,
      .event_handler = text_http_event_handler,
      .user_data = &response,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (client == NULL) {
    return ESP_FAIL;
  }
  esp_http_client_set_header(client, "X-Session-ID", DEVICE_SESSION_ID);
  const esp_err_t request_result = esp_http_client_perform(client);
  const int status_code = esp_http_client_get_status_code(client);
  esp_http_client_cleanup(client);
  if (request_result != ESP_OK || status_code != 200) {
    return request_result != ESP_OK ? request_result : ESP_FAIL;
  }

  cJSON *root = cJSON_Parse(response_data);
  const cJSON *transcript = root != NULL
                                ? cJSON_GetObjectItemCaseSensitive(
                                      root, "transcript")
                                : NULL;
  const cJSON *answer =
      root != NULL ? cJSON_GetObjectItemCaseSensitive(root, "answer") : NULL;
  if (!cJSON_IsString(transcript) || transcript->valuestring == NULL ||
      !cJSON_IsString(answer) || answer->valuestring == NULL) {
    cJSON_Delete(root);
    return ESP_ERR_INVALID_RESPONSE;
  }
  const esp_err_t result = app_storage_append_conversation(
      transcript->valuestring, answer->valuestring);
  cJSON_Delete(root);
  return result;
}
