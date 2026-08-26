#pragma once

#include <stdbool.h>

#include "esp_err.h"

// 挂载microSD。失败时不会格式化存储卡。
esp_err_t app_storage_init(void);

// 以追加方式保存一轮对话，不覆盖既有记录。
esp_err_t app_storage_append_conversation(const char *transcript,
                                          const char *answer);
