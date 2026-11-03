// Minimal ESP-IDF peer for an external low-voltage LED, not a mains relay.
#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "sdkconfig.h"
#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"

typedef struct { char id[64]; uint32_t value; } saved_command_t;
static saved_command_t saved;
static nvs_handle_t store;
static httpd_handle_t server;
static bool output_ready;
static esp_err_t fail(httpd_req_t *req, const char *status, const char *error) {
  httpd_resp_set_status(req, status); httpd_resp_set_type(req, "application/json");
  return httpd_resp_sendstr(req, error);
}
static bool authorized(httpd_req_t *req) {
  char header[144], expected[144];
  size_t n = strlen(CONFIG_VOX_PEER_TOKEN);
  if (n < 16 || n > 128 || httpd_req_get_hdr_value_str(req, "Authorization", header, sizeof(header)) != ESP_OK) return false;
  snprintf(expected, sizeof(expected), "Bearer %s", CONFIG_VOX_PEER_TOKEN);
  size_t count = strlen(expected);
  if (strlen(header) != count) return false;
  unsigned diff = 0; for (size_t i = 0; i < count; ++i) diff |= (unsigned char)header[i] ^ (unsigned char)expected[i];
  return diff == 0;
}
static esp_err_t state_reply(httpd_req_t *req, bool command) {
  cJSON *r = cJSON_CreateObject();
  if (!r) return fail(req, "500 Internal Server Error", "{\"error\":\"memory\"}");
  cJSON_AddBoolToObject(r, "value", saved.value != 0);
  if (command) cJSON_AddStringToObject(r, "command_id", saved.id);
  char *json = cJSON_PrintUnformatted(r); cJSON_Delete(r);
  if (!json) return fail(req, "500 Internal Server Error", "{\"error\":\"memory\"}");
  httpd_resp_set_type(req, "application/json");
  esp_err_t result = httpd_resp_sendstr(req, json); free(json); return result;
}
static esp_err_t state_handler(httpd_req_t *req) {
  if (!authorized(req)) return fail(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
  if (!output_ready) return fail(req, "503 Service Unavailable", "{\"error\":\"gpio_not_configured\"}");
  return state_reply(req, false);
}
static esp_err_t command_handler(httpd_req_t *req) {
  if (!authorized(req)) return fail(req, "401 Unauthorized", "{\"error\":\"unauthorized\"}");
  if (!output_ready) return fail(req, "503 Service Unavailable", "{\"error\":\"gpio_not_configured\"}");
  if (req->content_len < 2 || req->content_len > 256) return fail(req, "400 Bad Request", "{\"error\":\"size\"}");
  char buf[257]; size_t used = 0;
  while (used < req->content_len) {
    int n = httpd_req_recv(req, buf + used, req->content_len - used);
    if (n <= 0) return ESP_FAIL;
    used += (size_t)n;
  }
  buf[used] = 0;
  cJSON *r = strstr(buf, "\\u0000") ? NULL : cJSON_ParseWithLengthOpts(buf, used + 1, NULL, true);
  const cJSON *id = cJSON_GetObjectItemCaseSensitive(r, "command_id"), *value = cJSON_GetObjectItemCaseSensitive(r, "value");
  bool valid = cJSON_IsObject(r) && cJSON_GetArraySize(r) == 2 && cJSON_IsString(id) &&
      *id->valuestring && strlen(id->valuestring) < 64 && cJSON_IsBool(value);
  if (valid) for (const unsigned char *p = (const unsigned char *)id->valuestring; *p; ++p)
    if (!isalnum(*p) && *p != '-' && *p != '_') valid = false;
  if (!valid) { cJSON_Delete(r); return fail(req, "400 Bad Request", "{\"error\":\"fields\"}"); }
  saved_command_t next = {0}; strcpy(next.id, id->valuestring); next.value = cJSON_IsTrue(value);
  cJSON_Delete(r);
  if (!strcmp(saved.id, next.id)) {
    if (saved.value != next.value) return fail(req, "409 Conflict", "{\"error\":\"same_id_different_value\"}");
    return state_reply(req, true);
  }
  // Persist a SET state (not a TOGGLE); retrying the last ID does not repeat an edge.
  if (nvs_set_blob(store, "last", &next, sizeof(next)) != ESP_OK || nvs_commit(store) != ESP_OK)
    return fail(req, "500 Internal Server Error", "{\"error\":\"storage\"}");
  if (gpio_set_level(CONFIG_VOX_PEER_GPIO, next.value) != ESP_OK) {
    output_ready = false;
    return fail(req, "500 Internal Server Error", "{\"error\":\"gpio\"}");
  }
  saved = next; return state_reply(req, true);
}
static void network_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
  (void)arg;
  if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) esp_wifi_connect();
  if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *ip = data;
    ESP_LOGI("peer", "IP=" IPSTR, IP2STR(&ip->ip_info.ip));
    if (!server) {
      httpd_config_t cfg = HTTPD_DEFAULT_CONFIG(); cfg.recv_wait_timeout = 3; cfg.send_wait_timeout = 3;
      if (httpd_start(&server, &cfg) == ESP_OK) {
        httpd_uri_t get = {.uri="/state", .method=HTTP_GET, .handler=state_handler};
        httpd_uri_t post = {.uri="/command", .method=HTTP_POST, .handler=command_handler};
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
        ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
      }
    }
  }
}
void app_main(void) {
  ESP_ERROR_CHECK(nvs_flash_init());
  ESP_ERROR_CHECK(nvs_open("vox_peer", NVS_READWRITE, &store));
  size_t n = sizeof(saved);
  if (nvs_get_blob(store, "last", &saved, &n) != ESP_OK || n != sizeof(saved) ||
      !memchr(saved.id, 0, sizeof(saved.id)) || saved.value > 1) memset(&saved, 0, sizeof(saved));
  int pin = CONFIG_VOX_PEER_GPIO;
  if (pin >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(pin)) {
    gpio_config_t gpio = {.pin_bit_mask=1ULL << pin, .mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&gpio));
    ESP_ERROR_CHECK(gpio_set_level(CONFIG_VOX_PEER_GPIO, saved.value)); output_ready = true;
  }
  ESP_ERROR_CHECK(esp_netif_init()); ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT(); ESP_ERROR_CHECK(esp_wifi_init(&init));
  ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, network_event, NULL));
  ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, network_event, NULL));
  wifi_config_t wifi = {0};
  snprintf((char *)wifi.sta.ssid, sizeof(wifi.sta.ssid), "%s", CONFIG_VOX_PEER_SSID);
  snprintf((char *)wifi.sta.password, sizeof(wifi.sta.password), "%s", CONFIG_VOX_PEER_PASSWORD);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi)); ESP_ERROR_CHECK(esp_wifi_start());
}
