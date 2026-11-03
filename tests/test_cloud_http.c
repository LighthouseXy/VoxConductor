// Execute the production transport with deterministic local SDK fixtures.
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_cloud_http.h"
#include "freertos/task.h"

struct http_client { int status, timeout; };
static int init_count, open_count, cleanup_count, write_count, header_count;
static int fail_open, fail_write, fail_header, header_waits, partial_write;
static bool cancelled, fail_init, persistent = true, complete, tail_stuck;
static int64_t now_us;
static _Atomic bool *cancel_on_header;
static const char *URL = "https://example.invalid/chat";
bool app_cloud_is_cancelled(void) { return cancelled; }
int64_t esp_timer_get_time(void) { return now_us; }
void vTaskDelay(TickType_t ticks) { now_us += (int64_t)ticks * 1000; }
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *c) {
  assert(c->method == HTTP_METHOD_POST && c->crt_bundle_attach && c->disable_auto_redirect);
  assert(!strcmp(c->url, URL));
  ++init_count;
  return fail_init ? NULL : calloc(1, sizeof(struct http_client));
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *name, const char *value) {
  assert(c && name && value); return ESP_OK;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms) {
  c->timeout = ms; return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length) {
  assert(c && length >= 0); ++open_count;
  if (fail_open) { --fail_open; return ESP_FAIL; }
  return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
  assert(c); ++cleanup_count; free(c); return ESP_OK;
}
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int length) {
  assert(c && data && length > 0 && length <= 2048); ++write_count;
  if (fail_write) return -1;
  return partial_write && length > partial_write ? partial_write : length;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c) {
  ++header_count;
  if (cancel_on_header) atomic_store(cancel_on_header, true);
  if (fail_header) return ESP_FAIL;
  // Match IDF's reset on each fetch; the status line arrived only once.
  c->status = header_count == 1 ? 200 : -1;
  if (header_waits) { --header_waits; now_us += c->timeout * 1000; return -ESP_ERR_HTTP_EAGAIN; }
  return 0;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->status; }
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t c) { assert(c); return persistent; }
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { assert(c); return complete; }
int esp_http_client_read(esp_http_client_handle_t c, char *data, int length) {
  assert(c && data && length > 0);
  now_us += c->timeout * 1000;
  if (tail_stuck) return -ESP_ERR_HTTP_EAGAIN;
  complete = true; return 0;
}
static void reset(void) {
  init_count = open_count = cleanup_count = write_count = header_count = 0;
  fail_open = fail_write = fail_header = header_waits = partial_write = 0;
  cancelled = fail_init = complete = tail_stuck = false; persistent = true;
  now_us = 0; cancel_on_header = NULL;
}
static esp_err_t open_client(esp_http_client_handle_t *c) {
  return app_cloud_http_open(URL, "test-key", 5, 2000, NULL, c);
}
int main(void) {
  esp_http_client_handle_t c = NULL;
  reset();
  assert(app_cloud_http_open("http://example.invalid", "test-key", 5, 2000, NULL, &c) == ESP_ERR_INVALID_ARG);
  assert(app_cloud_http_open(URL, "", 5, 2000, NULL, &c) == ESP_ERR_INVALID_ARG);
  cancelled = true; assert(open_client(&c) == ESP_ERR_INVALID_STATE && !c);
  cancelled = false; fail_init = true; assert(open_client(&c) == ESP_ERR_NO_MEM && !c);
  fail_init = false;
  assert(open_client(&c) == ESP_OK);
  assert(app_cloud_http_reusable(c));
  assert(open_client(&c) == ESP_OK && init_count == 2 && open_count == 2);
  fail_open = 1;
  assert(open_client(&c) == ESP_OK && init_count == 3 && cleanup_count == 1);
  int opens = open_count;
  partial_write = 2;
  assert(app_cloud_http_write(c, "hello", 5, NULL) == ESP_OK && write_count == 3);
  fail_write = 1;
  assert(app_cloud_http_write(c, "hello", 5, NULL) == ESP_FAIL && opens == open_count);
  _Atomic bool abort = true;
  int writes = write_count;
  assert(app_cloud_http_write(c, "hello", 5, &abort) == ESP_ERR_INVALID_STATE && writes == write_count);
  atomic_store(&abort, false);
  int status;
  header_waits = 2;
  assert(app_cloud_http_headers(c, &abort, &status) == ESP_OK && status == 200 && header_count == 3);
  header_count = 0; header_waits = 100;
  assert(app_cloud_http_headers(c, NULL, &status) == ESP_ERR_TIMEOUT && header_count < 32);
  header_count = 0; cancel_on_header = &abort;
  assert(app_cloud_http_headers(c, &abort, &status) == ESP_ERR_INVALID_STATE && header_count == 1);
  atomic_store(&abort, false); cancel_on_header = NULL; fail_header = 1;
  assert(app_cloud_http_headers(c, NULL, &status) == ESP_FAIL);
  persistent = false; assert(!app_cloud_http_reusable(c));
  persistent = true; complete = false; tail_stuck = true;
  int64_t start = now_us;
  assert(!app_cloud_http_reusable(c) && now_us - start < 500000);
  cancelled = true; complete = true; assert(!app_cloud_http_reusable(c));
  cancelled = false; fail_open = 2;
  assert(open_client(&c) == ESP_FAIL && !c && cleanup_count == 3);
  puts("PASS: HTTP reuse, stale connection, partial writes, split headers, timeout and cancellation (SDK fake)");
}
