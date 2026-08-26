#include "app_storage.h"

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
#define SD_PIN_CS GPIO_NUM_2
#define SD_PIN_SCK GPIO_NUM_13
#define SD_PIN_MOSI GPIO_NUM_18
#define SD_PIN_MISO GPIO_NUM_21

static const char *TAG = "app_storage";
static bool s_ready;

esp_err_t app_storage_init(void) {
  const esp_vfs_fat_sdmmc_mount_config_t mount_config = {
      .format_if_mount_failed = false,
      .max_files = 3,
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

  ESP_LOGI(TAG, "本轮对话已写入%s/conversations.txt", SD_MOUNT_POINT);
  return ESP_OK;
}
