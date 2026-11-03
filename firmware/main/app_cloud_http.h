#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include "esp_http_client.h"

// 句柄由调用任务独占；仅在同一URL/密钥的一轮请求中复用，结束后cleanup。
// out首次为NULL；旧连接失效时只重试请求头，正文发送后不自动重发。
esp_err_t app_cloud_http_open(const char *url, const char *key, size_t length,
                             unsigned timeout_ms, const _Atomic bool *abort,
                             esp_http_client_handle_t *out);
bool app_cloud_http_cancelled(const _Atomic bool *abort);
esp_err_t app_cloud_http_write(esp_http_client_handle_t client, const void *data,
                              size_t length, const _Atomic bool *abort);
// 等待HTTP响应头时也可取消；成功不代表HTTP状态为200，由业务层检查。
esp_err_t app_cloud_http_headers(esp_http_client_handle_t client,
                                const _Atomic bool *abort, int *status);
// SSE结束后收完HTTP尾部才允许复用；false时调用者关闭连接，保留已完成回答。
bool app_cloud_http_reusable(esp_http_client_handle_t client);

// Absolute monotonic budget; zero preserves the background transport limits.
esp_err_t app_cloud_http_open_until(const char *url, const char *key, size_t length,
    unsigned timeout_ms, const _Atomic bool *abort, esp_http_client_handle_t *out, int64_t until);
esp_err_t app_cloud_http_write_until(esp_http_client_handle_t client, const void *data,
    size_t length, const _Atomic bool *abort, int64_t until);
esp_err_t app_cloud_http_headers_until(esp_http_client_handle_t client,
    const _Atomic bool *abort, int *status, int64_t until);
