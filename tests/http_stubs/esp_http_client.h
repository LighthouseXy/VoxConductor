#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#define ESP_ERR_HTTP_EAGAIN 0x7007
#define HTTP_METHOD_POST 1
typedef struct http_client *esp_http_client_handle_t;
typedef struct {
  const char *url;
  int method, timeout_ms;
  int (*crt_bundle_attach)(void *);
  bool disable_auto_redirect;
  int buffer_size, buffer_size_tx;
} esp_http_client_config_t;
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config);
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *name, const char *value);
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int ms);
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int length);
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c);
int esp_http_client_write(esp_http_client_handle_t c, const char *data, int length);
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c);
int esp_http_client_get_status_code(esp_http_client_handle_t c);
bool esp_http_client_is_persistent_connection(esp_http_client_handle_t c);
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c);
int esp_http_client_read(esp_http_client_handle_t c, char *data, int length);
