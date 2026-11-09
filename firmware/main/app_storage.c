/*
 * 存储适配：挂载SD，保存轮次事件/上下文，调用有界历史索引。
 * 挂载失败不格式化卡；磁盘文字与执行授权分离。
 * 异步处理不能保留事件中临时text指针，持久化失败需明确返回。
 */
#include "app_storage.h"
#include "app_log.h"
#include "app_history_store.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "driver/sdspi_host.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"

#define SD_HOST SPI3_HOST
#define SD_MOUNT_POINT "/sdcard"
#define SD_PIN_CS GPIO_NUM_48
#define SD_PIN_SCK GPIO_NUM_21
#define SD_PIN_MOSI GPIO_NUM_47
#define SD_PIN_MISO GPIO_NUM_14

static const char *TAG = "app_storage";
static bool s_ready;
bool app_storage_capacity(unsigned *active_bytes,unsigned *archives) {
  return s_ready && app_history_capacity(SD_MOUNT_POINT,active_bytes,archives);
}

void app_storage_record_event(const app_turn_t *turn, const app_turn_event_t *event) {
  if (!APP_STORAGE_MEMORY_ENABLED) return;
  // 不在连续播放期间写盘；终态携带生成/播放状态和PCM提交量。
  if (event->type != TURN_USER_TEXT && event->type != TURN_ASSISTANT_TEXT &&
      event->type != TURN_DONE && event->type != TURN_ERROR && event->type != TURN_CANCEL) return;
  time_t now = time(NULL);
  if (!s_ready || !app_history_event(SD_MOUNT_POINT, turn, event,
                                     now >= 1704067200 ? (long long)now : 0))
    ESP_LOGW(TAG, "[%s] 结构化记录未保存（SD不可用、写入失败或封存卷已满，请导出）", app_log_id(turn->id));
}

void app_storage_restore_context(app_conversation_t *history) {
  if (!APP_STORAGE_MEMORY_ENABLED) {
    ESP_LOGI(TAG, "文字记忆已关闭，不恢复上下文");
    return;
  }
  if (!s_ready) {
    ESP_LOGW(TAG, "SD不可用，从空上下文开始");
    return;
  }
  int result = app_history_restore(SD_MOUNT_POINT, history);
  if (result > 0) ESP_LOGI(TAG, "已从SD恢复最近%u轮上下文", (unsigned)history->count);
  else if (result == 0) ESP_LOGI(TAG, "尚无上下文快照，从新对话开始");
  else ESP_LOGW(TAG, "上下文快照无效或读取失败，从空上下文开始");
}

app_commit_result_t app_storage_save_context(const app_conversation_t *history) {
  if (!APP_STORAGE_MEMORY_ENABLED)
    return (app_commit_result_t){APP_COMMIT_PERMANENT_FAILURE, 0, "storage_disabled"};
  if (!s_ready)
    return (app_commit_result_t){APP_COMMIT_RETRYABLE_FAILURE, 0, "storage_unavailable"};
  if (app_history_deletion_pending(SD_MOUNT_POINT))
    return (app_commit_result_t){APP_COMMIT_RETRYABLE_FAILURE, 0, "deletion_pending"};
  app_commit_result_t result = app_history_commit(SD_MOUNT_POINT, history);
  if (result.status != APP_COMMIT_COMMITTED)
    ESP_LOGW(TAG, "上下文提交未确认：status=%d revision=%u reason=%s",
             result.status, result.revision, result.reason);
  return result;
}

cJSON *app_storage_task_receipt(const cJSON *expected) {
  return APP_STORAGE_MEMORY_ENABLED && s_ready ?
      app_history_task_receipt(SD_MOUNT_POINT, expected) : NULL;
}

bool app_storage_persist_context(const app_conversation_t *history) {
  return app_storage_save_context(history).status == APP_COMMIT_COMMITTED;
}
bool app_storage_erase_history(const app_conversation_t *replacement) {
  return APP_STORAGE_MEMORY_ENABLED && s_ready && app_history_erase(SD_MOUNT_POINT, replacement);
}
bool app_storage_forget_memory(const app_conversation_t *replacement, const cJSON *notes) {
  return APP_STORAGE_MEMORY_ENABLED && s_ready && app_history_forget(SD_MOUNT_POINT, replacement, notes);
}
bool app_storage_retry_forget(app_conversation_t *history) {
  return APP_STORAGE_MEMORY_ENABLED && s_ready && app_history_retry_forget(SD_MOUNT_POINT, history);
}
bool app_storage_deletion_pending(void) {
  return APP_STORAGE_MEMORY_ENABLED && s_ready && app_history_deletion_pending(SD_MOUNT_POINT);
}

cJSON *app_storage_search_history(const char *keyword) {
  if (APP_STORAGE_MEMORY_ENABLED && s_ready) return app_history_search(SD_MOUNT_POINT, keyword);
  return cJSON_Parse("{\"error\":\"storage_unavailable_or_disabled\"}");
}
cJSON *app_storage_history_query(const char *keyword, long before_offset, const char *record_id) {
  if (APP_STORAGE_MEMORY_ENABLED && s_ready) return app_history_query(SD_MOUNT_POINT, keyword, before_offset, record_id);
  return cJSON_Parse("{\"error\":\"storage_unavailable_or_disabled\"}");
}
cJSON *app_storage_history_query_filtered(const char *keyword, long before_offset,
                                          const char *record_id, const app_history_filter_t *filter) {
  if (APP_STORAGE_MEMORY_ENABLED && s_ready) return app_history_query_filtered(SD_MOUNT_POINT, keyword, before_offset, record_id, filter);
  return cJSON_Parse("{\"error\":\"storage_unavailable_or_disabled\"}");
}
void app_storage_index_tick(void) {
  if (APP_STORAGE_MEMORY_ENABLED && s_ready) (void)app_history_index_tick(SD_MOUNT_POINT);
}

esp_err_t app_storage_init(void) {
  const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      // History, task storage and the bounded media importer can coexist.
      .max_files = 8,
      .allocation_unit_size = 16 * 1024,
  };

  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = SD_HOST;
  host.max_freq_khz = 10000;

  const spi_bus_config_t bus_config = {
      .mosi_io_num = SD_PIN_MOSI,
      .miso_io_num = SD_PIN_MISO,
      .sclk_io_num = SD_PIN_SCK,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4096,
  };

  esp_err_t result = spi_bus_initialize(SD_HOST, &bus_config, SPI_DMA_CH_AUTO);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "microSD SPI初始化失败：%s", esp_err_to_name(result));
    return result;
  }

  sdspi_device_config_t device_config = SDSPI_DEVICE_CONFIG_DEFAULT();
  device_config.host_id = SD_HOST;
  device_config.gpio_cs = SD_PIN_CS;

  sdmmc_card_t *card = NULL;
  result = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &device_config,
                                   &mount_config, &card);
  if (result != ESP_OK) {
    ESP_LOGE(TAG, "microSD挂载失败：%s", esp_err_to_name(result));
    spi_bus_free(SD_HOST);
    return result;
  }

  ESP_LOGI(TAG, "microSD挂载成功");
  s_ready = true;
  return ESP_OK;
}

esp_err_t app_storage_append_conversation(const char *transcript,
                                          const char *answer) {
  if (!s_ready) {
    ESP_LOGW(TAG, "microSD不可用，跳过对话记录");
    return ESP_ERR_INVALID_STATE;
  }

  if (transcript == NULL || answer == NULL) {
    return ESP_ERR_INVALID_ARG;
  }

  time_t now;
  struct tm time_info;
  time(&now);

  // SNTP提供UTC时间；当前产品时区为UTC+8。
  now += 8 * 60 * 60;
  gmtime_r(&now, &time_info);

  char timestamp[32] = {0};
  if (strftime(timestamp, sizeof(timestamp), "%Y-%m-%d %H:%M:%S",
               &time_info) == 0) {
    strcpy(timestamp, "unknown time");
  }

  FILE *file = fopen(SD_MOUNT_POINT "/conversations.txt", "a");
  if (file == NULL) {
    ESP_LOGE(TAG, "无法打开SD卡对话记录文件");
    return ESP_FAIL;
  }

  const int write_result = fprintf(file, "[%s]\nUSER: %s\nAI: %s\n\n",
                                   timestamp, transcript, answer);
  const int close_result = fclose(file);

  if (write_result < 0 || close_result != 0) {
    ESP_LOGE(TAG, "SD卡对话记录写入失败");
    return ESP_FAIL;
  }

  APP_LOG_DETAIL(TAG, "本轮对话已写入%s/conversations.txt", SD_MOUNT_POINT);
  return ESP_OK;
}

cJSON *app_storage_recent_history(long before) {
  return APP_STORAGE_MEMORY_ENABLED && s_ready ? app_history_recent(SD_MOUNT_POINT,before) : NULL;
}
