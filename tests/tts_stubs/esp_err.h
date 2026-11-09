#pragma once
#include "../speech_stubs/esp_err.h"
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_TIMEOUT 0x107
const char *esp_err_to_name(int error);
