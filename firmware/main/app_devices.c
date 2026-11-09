/*
 * 注册表中的局域网设备适配，仅允许配置过的ID/能力。
 * 不接受模型提供的任意URL/令牌，凭据不进入状态快照。
 */
#include "app_memory_alloc.h"
#include "app_devices.h"
#include "app_cloud.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_http_client.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#if defined(APP_DEVICE_HOST_TEST)
#include <device_registry.h>
#elif __has_include("device_registry.h")
#include "device_registry.h"
#else
#include "device_registry.example.h"
#endif
_Static_assert(sizeof(APP_DEVICES) / sizeof(APP_DEVICES[0]) <= 4, "maximum four peers");
static const app_device_t *find(const char *id) {
  if (!id || !*id) return NULL;
  for (size_t i = 0; i < sizeof(APP_DEVICES) / sizeof(APP_DEVICES[0]); ++i) {
    const app_device_t *d = &APP_DEVICES[i];
    if (!strcmp(id, d->id) && strlen(d->id) <= 32 && strlen(d->url) < 160 &&
        strlen(d->token) >= 16 && strlen(d->token) <= 128 && !strpbrk(d->token, "\r\n") &&
        (!strncmp(d->url, "http://", 7) || !strncmp(d->url, "https://", 8))) return d;
  }
  return NULL;
}
bool app_device_configured(const char *id) { return find(id) != NULL; }
cJSON *app_device_ids(void) {
  cJSON *ids = cJSON_CreateArray();
  if (!ids) return NULL;
  for (size_t i = 0; i < sizeof(APP_DEVICES) / sizeof(APP_DEVICES[0]); ++i) {
    const app_device_t *d = &APP_DEVICES[i];
    if (!app_device_configured(d->id)) continue;
    cJSON *id = cJSON_CreateString(d->id);
    if (!id || !cJSON_AddItemToArray(ids, id)) { cJSON_Delete(id); cJSON_Delete(ids); return NULL; }
  }
  return ids;
}
static unsigned remaining_ms(int64_t deadline) {
  int64_t left = (deadline - esp_timer_get_time()) / 1000;
  return left <= 0 ? 0 : left < 3000 ? (unsigned)left : 3000;
}
static bool admit_io(esp_http_client_handle_t client, int64_t deadline) {
  unsigned timeout = remaining_ms(deadline);
  return timeout && esp_http_client_set_timeout_ms(client, timeout) == ESP_OK;
}
char *app_device_request(const char *id, const char *command_id, bool value) {
  const app_device_t *d = find(id);
  if (!d || app_cloud_is_cancelled()) return app_psram_strdup("{\"error\":\"unconfigured_or_cancelled\"}");
  int64_t deadline = esp_timer_get_time() + 3000000;
  int64_t turn_deadline = app_cloud_deadline_us();
  if (turn_deadline > 0 && turn_deadline < deadline) deadline = turn_deadline;
  if (!remaining_ms(deadline)) return app_psram_strdup("{\"error\":\"deadline_expired_not_dispatched\"}");
  char url[180], auth[144];
  snprintf(url, sizeof(url), "%s/%s", d->url, command_id ? "command" : "state");
  snprintf(auth, sizeof(auth), "Bearer %s", d->token);
  cJSON *body = cJSON_CreateObject();
  char *json = NULL;
  if (command_id) {
    if (!body || strlen(command_id) >= 64 || !cJSON_AddStringToObject(body, "command_id", command_id) ||
        !cJSON_AddBoolToObject(body, "value", value)) { cJSON_Delete(body); return NULL; }
    json = cJSON_PrintUnformatted(body);
    if (!json) { cJSON_Delete(body); return NULL; }
  }
  cJSON_Delete(body);
  esp_http_client_config_t config = {.url=url, .timeout_ms=(int)remaining_ms(deadline), .disable_auto_redirect=true,
    .method=command_id ? HTTP_METHOD_POST : HTTP_METHOD_GET, .crt_bundle_attach=esp_crt_bundle_attach,
    .buffer_size=512, .buffer_size_tx=512};
  esp_http_client_handle_t client = esp_http_client_init(&config);
  char response[1025]; size_t used = 0; bool ok = client != NULL;
  if (ok) ok = esp_http_client_set_header(client, "Authorization", auth) == ESP_OK &&
      esp_http_client_set_header(client, "Content-Type", "application/json") == ESP_OK;
  if (ok) ok = admit_io(client, deadline) && esp_http_client_open(client, json ? (int)strlen(json) : 0) == ESP_OK;
  if (ok && json) ok = !app_cloud_is_cancelled() && admit_io(client, deadline) &&
      esp_http_client_write(client, json, strlen(json)) == (int)strlen(json);
  free(json);
  if (ok) ok = !app_cloud_is_cancelled() && admit_io(client, deadline) &&
      esp_http_client_fetch_headers(client) >= 0 && esp_http_client_get_status_code(client) == 200;
  while (ok && used < 1024) {
    if (app_cloud_is_cancelled() || !admit_io(client, deadline)) { ok = false; break; }
    int n = esp_http_client_read(client, response + used, 1024 - used);
    if (n < 0) { ok = false; break; }
    if (!n) { ok = esp_http_client_is_complete_data_received(client); break; }
    used += (size_t)n;
    // Accept a complete late ACK as evidence; never renew the I/O budget.
    if (esp_http_client_is_complete_data_received(client)) break;
  }
  if (used == 1024) ok = false;
  if (client) esp_http_client_cleanup(client);
  response[used] = 0;
  cJSON *reply = ok ? cJSON_ParseWithLengthOpts(response, used + 1, NULL, true) : NULL;
  const cJSON *state = cJSON_GetObjectItemCaseSensitive(reply, "value");
  const cJSON *ack = cJSON_GetObjectItemCaseSensitive(reply, "command_id");
  ok = cJSON_IsObject(reply) && cJSON_IsBool(state) &&
      (!command_id || (cJSON_IsString(ack) && !strcmp(ack->valuestring, command_id) && cJSON_IsTrue(state) == value));
  cJSON *out = cJSON_CreateObject();
  if (ok) {
    cJSON_AddStringToObject(out, "device_id", id);
    cJSON_AddBoolToObject(out, "value", cJSON_IsTrue(state));
    cJSON_AddStringToObject(out, "status", command_id ? "acknowledged" : "observed");
  } else cJSON_AddStringToObject(out, "error", command_id ? "execution_unknown_do_not_retry_automatically" : "device_unavailable");
  char *result = out ? cJSON_PrintUnformatted(out) : NULL;
  cJSON_Delete(reply); cJSON_Delete(out);
  return result;
}
