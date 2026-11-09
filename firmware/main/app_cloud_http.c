/*
 * 云端HTTP传输：集中连接、短轮询和取消边界，不解析模型/工具内容。
 * 连接只在当前调用任务中存活，避免后台摘要或TTS同时持有同一TLS句柄。
 */
#include "app_cloud_http.h"
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include "esp_crt_bundle.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// 取消状态由语音协调器持有；此模块仅观察，不跨任务销毁连接。
extern bool app_cloud_is_cancelled(void);

bool app_cloud_http_cancelled(const _Atomic bool *abort) {
  return app_cloud_is_cancelled() || (abort && atomic_load(abort));
}

static unsigned bounded_timeout(int64_t until, unsigned cap) {
  if (until <= 0) return cap;
  int64_t remaining = (until - esp_timer_get_time()) / 1000;
  return remaining <= 0 ? 0 : remaining < cap ? (unsigned)remaining : cap;
}

esp_err_t app_cloud_http_open_until(const char *url, const char *key, size_t length,
                             unsigned timeout_ms, const _Atomic bool *abort,
                             esp_http_client_handle_t *out, int64_t until) {
  if (!out || !url || !key || !*key || strlen(key) > 256 ||
      strncmp(url, "https://", 8) || length > INT_MAX || !timeout_ms)
    return ESP_ERR_INVALID_ARG;
  if (app_cloud_http_cancelled(abort) || time(NULL) < 1704067200)
    return ESP_ERR_INVALID_STATE;
  timeout_ms = bounded_timeout(until, timeout_ms);
  if (!timeout_ms) return ESP_ERR_TIMEOUT;
  if (*out) {
    esp_http_client_set_timeout_ms(*out, timeout_ms);
    esp_err_t result = esp_http_client_open(*out, (int)length);
    if (result == ESP_OK) return bounded_timeout(until, 1) ? result : ESP_ERR_TIMEOUT;
    // 尚未发送正文，服务端不会执行完整模型请求。旧连接只重连一次。
    esp_http_client_cleanup(*out);
    *out = NULL;
    if (app_cloud_http_cancelled(abort)) return ESP_ERR_INVALID_STATE;
  }
  timeout_ms = bounded_timeout(until, timeout_ms);
  if (!timeout_ms) return ESP_ERR_TIMEOUT;
  const esp_http_client_config_t config = {
    .url = url, .method = HTTP_METHOD_POST, .timeout_ms = timeout_ms,
    .crt_bundle_attach = esp_crt_bundle_attach, .disable_auto_redirect = true,
    .buffer_size = 2048, .buffer_size_tx = 2048,
  };
  esp_http_client_handle_t client = esp_http_client_init(&config);
  if (!client) return ESP_ERR_NO_MEM;
  char authorization[272];
  snprintf(authorization, sizeof(authorization), "Bearer %s", key);
  esp_err_t result = esp_http_client_set_header(client, "Authorization", authorization);
  if (result == ESP_OK)
    result = esp_http_client_set_header(client, "Content-Type", "application/json");
  if (result == ESP_OK && app_cloud_http_cancelled(abort)) result = ESP_ERR_INVALID_STATE;
  if (result == ESP_OK) result = esp_http_client_open(client, (int)length);
  if (result == ESP_OK && !bounded_timeout(until, 1)) result = ESP_ERR_TIMEOUT;
  if (result != ESP_OK) { esp_http_client_cleanup(client); return result; }
  *out = client;
  return ESP_OK;
}

esp_err_t app_cloud_http_write_until(esp_http_client_handle_t client, const void *data,
                              size_t length, const _Atomic bool *abort, int64_t until) {
  if (!client || (!data && length)) return ESP_ERR_INVALID_ARG;
  const char *p = data;
  while (length) {
    if (app_cloud_http_cancelled(abort)) return ESP_ERR_INVALID_STATE;
    unsigned timeout = bounded_timeout(until, 1000);
    if (!timeout) return ESP_ERR_TIMEOUT;
    esp_http_client_set_timeout_ms(client, timeout);
    size_t chunk = length > 2048 ? 2048 : length;
    int written = esp_http_client_write(client, p, (int)chunk);
    // SDK写失败时可能已有部分正文到达，不能从头重发。
    if (written <= 0 || (size_t)written > chunk) return ESP_FAIL;
    p += written;
    length -= (size_t)written;
  }
  return bounded_timeout(until, 1) ? ESP_OK : ESP_ERR_TIMEOUT;
}

esp_err_t app_cloud_http_headers_until(esp_http_client_handle_t client,
                                const _Atomic bool *abort, int *status, int64_t until) {
  if (!client || !status) return ESP_ERR_INVALID_ARG;
  *status = -1;
  esp_http_client_set_timeout_ms(client, 1000);
  int64_t deadline = esp_timer_get_time() + 30000000;
  if (until > 0 && until < deadline) deadline = until;
  while (!app_cloud_http_cancelled(abort)) {
    if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
    unsigned timeout = bounded_timeout(deadline, 1000);
    if (!timeout) return ESP_ERR_TIMEOUT;
    esp_http_client_set_timeout_ms(client, timeout);
    int64_t headers = esp_http_client_fetch_headers(client);
    // IDF 6.0会在每次fetch_headers入口重置status_code。若状态行和其余
    // 响应头分两次到达，短轮询之间保留已解析的状态码，避免误报HTTP=-1。
    int received_status = esp_http_client_get_status_code(client);
    if (received_status > 0) *status = received_status;
    if (headers >= 0) return app_cloud_http_cancelled(abort) ? ESP_ERR_INVALID_STATE :
        bounded_timeout(deadline, 1) ? ESP_OK : ESP_ERR_TIMEOUT;
    if (headers != -ESP_ERR_HTTP_EAGAIN) return ESP_FAIL;
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  return ESP_ERR_INVALID_STATE;
}

bool app_cloud_http_reusable(esp_http_client_handle_t client) {
  if (!client || !esp_http_client_is_persistent_connection(client)) return false;
  // [DONE]结束的是SSE，HTTP的最后一个chunk/trailer可能尚未读到。
  int64_t deadline = esp_timer_get_time() + 300000;
  size_t drained = 0;
  esp_http_client_set_timeout_ms(client, 100);
  while (!esp_http_client_is_complete_data_received(client)) {
    if (app_cloud_is_cancelled() || esp_timer_get_time() >= deadline || drained >= 2048)
      return false;
    char tail[128];
    int n = esp_http_client_read(client, tail, sizeof(tail));
    if (n == -ESP_ERR_HTTP_EAGAIN || n == 0) {
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (n < 0) return false;
    drained += (size_t)n;
  }
  return !app_cloud_is_cancelled();
}

esp_err_t app_cloud_http_open(const char *url, const char *key, size_t length,
    unsigned timeout_ms, const _Atomic bool *abort, esp_http_client_handle_t *out) {
  return app_cloud_http_open_until(url, key, length, timeout_ms, abort, out, 0);
}
esp_err_t app_cloud_http_write(esp_http_client_handle_t client, const void *data,
    size_t length, const _Atomic bool *abort) {
  return app_cloud_http_write_until(client, data, length, abort, 0);
}
esp_err_t app_cloud_http_headers(esp_http_client_handle_t client,
    const _Atomic bool *abort, int *status) {
  return app_cloud_http_headers_until(client, abort, status, 0);
}
