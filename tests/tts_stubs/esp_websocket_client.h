#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef const char *esp_event_base_t;
enum { WEBSOCKET_EVENT_ANY = -1, WEBSOCKET_EVENT_DATA, WEBSOCKET_EVENT_ERROR, WEBSOCKET_EVENT_DISCONNECTED, WEBSOCKET_EVENT_CLOSED };
typedef struct {
  int op_code; bool fin; int payload_len, payload_offset, data_len; const char *data_ptr;
  struct { int esp_ws_handshake_status_code, esp_tls_last_esp_err; } error_handle;
} esp_websocket_event_data_t;
typedef void (*test_ws_callback_t)(void *, esp_event_base_t, int32_t, void *);
typedef struct test_ws *esp_websocket_client_handle_t;
typedef struct {
  const char *uri, *headers;
  int (*crt_bundle_attach)(void *);
  bool disable_auto_reconnect;
  unsigned task_stack, task_prio, buffer_size, network_timeout_ms;
} esp_websocket_client_config_t;
esp_websocket_client_handle_t esp_websocket_client_init(const esp_websocket_client_config_t *config);
esp_err_t esp_websocket_register_events(esp_websocket_client_handle_t client, int event, test_ws_callback_t callback, void *context);
esp_err_t esp_websocket_client_start(esp_websocket_client_handle_t client);
int esp_websocket_client_send_text(esp_websocket_client_handle_t client, const char *data, int length, unsigned wait);
esp_err_t esp_websocket_client_stop(esp_websocket_client_handle_t client);
esp_err_t esp_websocket_client_destroy(esp_websocket_client_handle_t client);
