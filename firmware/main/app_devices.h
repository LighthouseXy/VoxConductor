#pragma once
#include <stdbool.h>
#include "cJSON.h"
typedef struct { const char *id, *url, *token; } app_device_t;
bool app_device_configured(const char *id);
// Public capability IDs only, never endpoint URLs or authentication secrets.
cJSON *app_device_ids(void);
// Fixed registry only. Never accepts model-supplied URLs or paths. Caller frees.
char *app_device_request(const char *id, const char *command_id, bool value);
