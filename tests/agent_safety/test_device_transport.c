#include "app_devices.h"
#include "app_cloud.h"
#include "esp_http_client.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct http_client { unsigned timeout; size_t offset; };
static int64_t now_us, deadline;
static bool cancelled, drip, expire_on_open, late_reply;
static unsigned opens, writes, cleanups;
static const char *payload = "{\"command_id\":\"action-1\",\"value\":true}";
int64_t esp_timer_get_time(void) { return now_us; }
int64_t app_cloud_deadline_us(void) { return deadline; }
bool app_cloud_is_cancelled(void) { return cancelled; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
  assert(!strcmp(config->url, "http://device.invalid/command"));
  assert(config->disable_auto_redirect && config->timeout_ms > 0 && config->timeout_ms <= 3000);
  return calloc(1, sizeof(struct http_client));
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *key, const char *value) {
  assert(c && key && value); return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) {
  assert(ms > 0 && ms <= 3000); c->timeout = (unsigned)ms; return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length) {
  assert(length > 0); ++opens;
  if (expire_on_open) now_us += c->timeout * 1000;
  return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int length) {
  assert(c && data); ++writes; return length;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) { assert(c); return 0; }
int esp_http_client_get_status_code(esp_http_client_handle_t c) { assert(c); return 200; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->offset == strlen(payload); }
int esp_http_client_read(esp_http_client_handle_t c, char *out, int size) {
  size_t count = strlen(payload) - c->offset;
  if (count > (size_t)size) count = (size_t)size;
  if (drip && count > 1) count = 1;
  now_us += drip || late_reply ? c->timeout * 1000 : 1000;
  memcpy(out, payload + c->offset, count); c->offset += count;
  return (int)count;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) { ++cleanups; free(c); return ESP_OK; }
static void expect(const char *needle) {
  char *reply = app_device_request("light", "action-1", true);
  assert(reply && strstr(reply, needle)); free(reply);
}
int main(void) {
  now_us = 1000; deadline = 1000;
  expect("not_dispatched"); assert(opens == 0 && writes == 0);
  puts("PASS expired turn admits no device connection or write");
  deadline = now_us + 500000; expire_on_open = true;
  expect("unknown"); assert(opens == 1 && writes == 0 && cleanups == 1);
  puts("PASS budget consumed by connect prevents dispatch; no retry");
  expire_on_open = false; deadline = now_us + 1000000;
  expect("acknowledged"); assert(writes == 1 && cleanups == 2);
  puts("PASS exact action ID and value ACK accepted and handle cleaned");
  late_reply = true; deadline = now_us + 500000;
  expect("acknowledged"); assert(now_us == deadline && writes == 2 && cleanups == 3);
  puts("PASS complete ACK at the deadline remains execution evidence");
  late_reply = false;
  payload = "{\"command_id\":\"old-action\",\"value\":true}";
  deadline = now_us + 1000000;
  expect("unknown"); assert(writes == 3 && cleanups == 4);
  puts("PASS stale ACK action ID cannot acknowledge the current action");
  payload = "{\"command_id\":\"action-1\",\"value\":true}";
  deadline = 0; drip = true; int64_t started = now_us;
  expect("unknown"); assert(now_us - started <= 3000000 && cleanups == 5 && writes == 4);
  puts("PASS slow response consumes one total device budget, not a renewed budget per byte");
}
