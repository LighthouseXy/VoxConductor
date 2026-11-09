#pragma once
#include <stdbool.h>

typedef struct {
  bool enabled;
  unsigned seconds_remaining;
  char url[64], message[96];
} app_media_web_status_t;
void app_media_web_init(void);
// Nonblocking request from UI; the worker manages the server and music lease.
void app_media_web_request(bool enabled);
void app_media_web_status(app_media_web_status_t *out);
