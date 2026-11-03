/*
 * 内嵌HTTP接口：静态管理页、受限媒体传输与诊断下载。
 * 控制接口只校验、入队、查回执，不在HTTP回调执行慢控制操作。
 * 文件名、长度和下载范围由设备端验证，不能信任浏览器预检查。
 */
// User-enabled LAN manager. Private text/configuration only via session-gated,
// bounded jobs; no credentials or arbitrary SD paths are exposed.
#include "app_media_web.h"
#include "app_control.h"
#include "app_http_range.h"
#include "app_desktop.h"
#include "app_wake.h"
#include "app_wake_diagnostic.h"
#include "app_barge_diagnostic.h"
#include "esp_lvgl_port.h"
#include "sdkconfig.h"

#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "app_media.h"
#include "app_memory_alloc.h"
#include "app_network.h"
#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WEB_SESSION_SECONDS 600
#define WEB_JSON_BYTES 768
#define WEB_CHUNK_BYTES 2048
#define WEB_TRANSFER_SECONDS 1800
#define WEB_TRANSFER_STALL_SECONDS 30

extern const unsigned char
    media_html_start[] asm("_binary_media_web_html_start");
extern const unsigned char media_html_end[] asm("_binary_media_web_html_end");

static httpd_handle_t s_server;
static _Atomic bool s_requested;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static app_media_web_status_t s_status;
static int64_t s_expires_us;

static void set_message(const char *message) {
  portENTER_CRITICAL(&s_status_lock);
  snprintf(s_status.message, sizeof(s_status.message), "%s", message);
  portEXIT_CRITICAL(&s_status_lock);
}

void app_media_web_status(app_media_web_status_t *out) {
  if (!out)
    return;
  portENTER_CRITICAL(&s_status_lock);
  *out = s_status;
  int64_t remaining = s_expires_us - esp_timer_get_time();
  out->seconds_remaining = out->enabled && remaining > 0
                               ? (unsigned)((remaining + 999999) / 1000000)
                               : 0;
  portEXIT_CRITICAL(&s_status_lock);
}

static bool transfer_live(int64_t deadline_us) {
  app_media_web_status_t status;
  app_media_web_status(&status);
  return status.enabled && status.seconds_remaining &&
         atomic_load(&s_requested) && esp_timer_get_time() < deadline_us;
}

// Only activity while management is open or successfully transferred bytes renew the
// idle timeout. Never revive an expired or manually disabled session.
static bool session_touch(void) {
  int64_t now_us = esp_timer_get_time();
  portENTER_CRITICAL(&s_status_lock);
  bool live =
      s_status.enabled && atomic_load(&s_requested) && now_us < s_expires_us;
  if (live)
    s_expires_us = now_us + WEB_SESSION_SECONDS * 1000000LL;
  portEXIT_CRITICAL(&s_status_lock);
  return live;
}

static esp_err_t fail(httpd_req_t *req, const char *status,
                      const char *message) {
  httpd_resp_set_status(req, status);
  httpd_resp_set_type(req, "text/plain; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_sendstr(req, message);
  // Close rejected bodies instead of retaining unread bytes on keep-alive.
  return ESP_FAIL;
}

static bool session_open(void) {
  // LAN management is intentionally unpaired; retain device-side on/off and expiry.
  return session_touch();
}

static esp_err_t json_reply(httpd_req_t *req, cJSON *root) {
  if (!root)
    return fail(req, "503 Service Unavailable", "内存不足");
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  if (!json)
    return fail(req, "503 Service Unavailable", "内存不足");
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  esp_err_t result = httpd_resp_sendstr(req, json);
  free(json);
  return result;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  httpd_resp_set_hdr(req, "Content-Security-Policy",
                     "default-src 'self'; script-src 'unsafe-inline'; "
                     "style-src 'unsafe-inline'; img-src 'self' blob: data:; "
                     "media-src 'self' blob:; connect-src 'self'; "
                     "frame-ancestors 'none'");
  size_t size = (size_t)(media_html_end - media_html_start);
  if (size && media_html_start[size - 1] == 0)
    --size;
  return httpd_resp_send(req, (const char *)media_html_start, size);
}

static esp_err_t wake_diagnostic_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  if (req->method == HTTP_POST) {
    // Fixed duration, no request body or arbitrary audio paths.
    if (req->content_len)
      return fail(req, "400 Bad Request", "诊断启动不接受请求内容");
    esp_err_t result = app_wake_diagnostic_start();
    if (result != ESP_OK)
      return fail(req, result == ESP_ERR_NO_MEM ? "503 Service Unavailable" : "409 Conflict",
                  result == ESP_ERR_NO_MEM ? "诊断缓冲不足" :
                  "请开启语音唤醒，停止音乐或对话，等待模型就绪；正在采集时不能重新开始");
  } else if (req->method == HTTP_DELETE) {
    if (req->content_len)
      return fail(req, "400 Bad Request", "清除诊断不接受请求内容");
    if (app_wake_diagnostic_clear() != ESP_OK)
      return fail(req, "503 Service Unavailable", "诊断忙，未清除，请重试");
  }
  app_wake_diagnostic_status_t status;
  if (!app_wake_diagnostic_status(&status))
    return fail(req, "503 Service Unavailable", "诊断忙，请重试");
  cJSON *root = cJSON_CreateObject();
  if (!root) return fail(req, "503 Service Unavailable", "内存不足");
  cJSON_AddBoolToObject(root, "available", app_wake_available());
  cJSON_AddBoolToObject(root, "enabled", app_wake_is_enabled());
  cJSON_AddBoolToObject(root, "input_allowed", app_wake_input_allowed());
  cJSON_AddBoolToObject(root, "active", status.active);
  cJSON_AddNumberToObject(root, "id", status.id);
  cJSON_AddNumberToObject(root, "samples", status.samples);
  cJSON_AddNumberToObject(root, "hits", status.hits);
  cJSON_AddNumberToObject(root, "clipped", status.clipped);
  cJSON_AddNumberToObject(root, "skipped_frames", status.skipped_frames);
  cJSON_AddNumberToObject(root, "raw_peak", status.raw_peak);
  cJSON_AddNumberToObject(root, "model_peak", status.model_peak);
  cJSON_AddNumberToObject(root, "elapsed_ms", status.elapsed_ms);
  cJSON_AddNumberToObject(root, "reason", status.reason);
  cJSON_AddNumberToObject(root, "rate", APP_WAKE_DIAGNOSTIC_RATE);
  cJSON_AddNumberToObject(root, "seconds", APP_WAKE_DIAGNOSTIC_SECONDS);
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
  cJSON_AddBoolToObject(root, "computer_barge_in", true);
#else
  cJSON_AddBoolToObject(root, "computer_barge_in", false);
#endif
  return json_reply(req, root);
}

static esp_err_t barge_diagnostic_handler(httpd_req_t *req) {
  if (!session_open()) return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  if (req->content_len) return fail(req, "400 Bad Request", "诊断不接受请求内容");
  if (req->method == HTTP_POST) {
    esp_err_t result = app_barge_diagnostic_start();
    if (result != ESP_OK)
      return fail(req, result == ESP_ERR_NO_MEM ? "503 Service Unavailable" : "409 Conflict",
          result == ESP_ERR_NO_MEM ? "诊断内存不足，请先清除旧的空闲唤醒录音" :
          "请在空闲时开启语音唤醒，结束其他诊断；固件需支持小冰小冰打断");
  } else if (req->method == HTTP_DELETE && app_barge_diagnostic_clear() != ESP_OK) {
    return fail(req, "503 Service Unavailable", "诊断忙，未清除，请重试");
  }
  app_barge_diagnostic_status_t status;
  if (!app_barge_diagnostic_status(&status)) return fail(req, "503 Service Unavailable", "诊断忙，请重试");
  cJSON *root = cJSON_CreateObject();
  if (!root) return fail(req, "503 Service Unavailable", "内存不足");
  cJSON_AddNumberToObject(root, "id", status.id);
  cJSON_AddNumberToObject(root, "state", status.state);
  cJSON_AddNumberToObject(root, "reason", status.reason);
  cJSON_AddNumberToObject(root, "elapsed_ms", status.elapsed_ms);
  cJSON_AddNumberToObject(root, "rate", BARGE_DIAG_RATE);
  cJSON_AddNumberToObject(root, "seconds", BARGE_DIAG_SECONDS);
  cJSON_AddNumberToObject(root, "clipped", status.clipped);
  cJSON_AddNumberToObject(root, "skipped", status.skipped);
  cJSON *tracks = cJSON_AddArrayToObject(root, "tracks");
  for (unsigned i = 0; i < BARGE_DIAG_TRACKS; ++i) {
    cJSON *track = cJSON_CreateObject();
    cJSON_AddNumberToObject(track, "samples", status.samples[i]);
    cJSON_AddNumberToObject(track, "peak", status.peaks[i]);
    cJSON_AddItemToArray(tracks, track);
  }
  static const char *names[BD_COUNT] = {"mic_reads", "mic", "aec", "queued", "drop", "processed", "hit",
      "stale", "debounce", "disabled", "ref_missing_samples", "ref_overflow", "request",
      "reject_epoch", "reject_state", "duplicate", "accepted", "handoff_race"};
  cJSON *counts = cJSON_AddObjectToObject(root, "counts");
  for (unsigned i = 0; i < BD_COUNT; ++i) cJSON_AddNumberToObject(counts, names[i], status.counts[i]);
#if CONFIG_VOX_WAKE_WORD
  cJSON_AddNumberToObject(root, "wake_gain", CONFIG_VOX_WAKE_GAIN);
  cJSON_AddNumberToObject(root, "wake_threshold_percent", CONFIG_VOX_WAKE_THRESHOLD_PERCENT);
#endif
#if CONFIG_VOX_ACOUSTIC_BARGE_IN
  cJSON_AddNumberToObject(root, "reference_delay_ms", CONFIG_VOX_AEC_DELAY_MS);
#endif
  return json_reply(req, root);
}

static void wav_u32(uint8_t *at, uint32_t value) {
  for (unsigned i = 0; i < 4; ++i) at[i] = (uint8_t)(value >> (8 * i));
}
static esp_err_t wake_audio_handler(httpd_req_t *req) {
  if (!session_open()) return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  char query[80], channel[8], id_text[11];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "channel", channel, sizeof(channel)) != ESP_OK ||
      (strcmp(channel, "raw") && strcmp(channel, "model")) ||
      httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK ||
      !id_text[0] || strspn(id_text, "0123456789") != strlen(id_text))
    return fail(req, "400 Bad Request", "诊断参数无效");
  errno = 0;
  uint64_t id = strtoull(id_text, NULL, 10);
  if (errno || !id || id > UINT32_MAX)
    return fail(req, "400 Bad Request", "诊断编号无效");
  app_wake_diagnostic_status_t status;
  if (!app_wake_diagnostic_status(&status))
    return fail(req, "503 Service Unavailable", "诊断忙，请重试");
  if (status.active || status.id != id || !status.samples)
    return fail(req, "409 Conflict", "采集未完成、无音频或录音已清除");
  uint32_t bytes = status.samples * sizeof(int16_t);
  uint8_t header[44] = {0};
  memcpy(header, "RIFF", 4); wav_u32(header + 4, bytes + 36);
  memcpy(header + 8, "WAVEfmt ", 8); wav_u32(header + 16, 16);
  header[20] = 1; header[22] = 1;
  wav_u32(header + 24, APP_WAKE_DIAGNOSTIC_RATE);
  wav_u32(header + 28, APP_WAKE_DIAGNOSTIC_RATE * 2);
  header[32] = 2; header[34] = 16;
  memcpy(header + 36, "data", 4); wav_u32(header + 40, bytes);
  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  uint8_t *buffer = app_psram_malloc(WEB_CHUNK_BYTES);
  if (!buffer) return fail(req, "503 Service Unavailable", "内存不足");
  esp_err_t result = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
  for (size_t offset = 0; result == ESP_OK && offset < bytes;) {
    size_t count = app_wake_diagnostic_read((unsigned)id, !strcmp(channel, "model"),
                                             offset, buffer, WEB_CHUNK_BYTES);
    if (!count) { result = ESP_FAIL; break; }
    result = httpd_resp_send_chunk(req, (const char *)buffer, count);
    offset += count;
    if (!session_touch()) result = ESP_FAIL;
  }
  free(buffer);
  if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
  return result;
}

static esp_err_t barge_audio_handler(httpd_req_t *req) {
  if (!session_open()) return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  char query[80], channel[2], id_text[11];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "track", channel, sizeof(channel)) != ESP_OK ||
      (channel[0] < '0' || channel[0] > '3' || channel[1]) ||
      httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK ||
      !id_text[0] || strspn(id_text, "0123456789") != strlen(id_text))
    return fail(req, "400 Bad Request", "诊断参数无效");
  errno = 0;
  uint64_t id = strtoull(id_text, NULL, 10);
  if (errno || !id || id > UINT32_MAX)
    return fail(req, "400 Bad Request", "诊断编号无效");
  unsigned track = (unsigned)(channel[0] - '0');
  app_barge_diagnostic_status_t status;
  if (!app_barge_diagnostic_status(&status))
    return fail(req, "503 Service Unavailable", "诊断忙，请重试");
  if (status.state != 3 || status.id != id || !status.samples[track])
    return fail(req, "409 Conflict", "采集未完成、无音频或录音已清除");
  uint32_t bytes = status.samples[track] * sizeof(int16_t);
  uint8_t header[44] = {0};
  memcpy(header, "RIFF", 4); wav_u32(header + 4, bytes + 36);
  memcpy(header + 8, "WAVEfmt ", 8); wav_u32(header + 16, 16);
  header[20] = 1; header[22] = 1;
  wav_u32(header + 24, BARGE_DIAG_RATE);
  wav_u32(header + 28, BARGE_DIAG_RATE * 2);
  header[32] = 2; header[34] = 16;
  memcpy(header + 36, "data", 4); wav_u32(header + 40, bytes);
  httpd_resp_set_type(req, "audio/wav");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  uint8_t *buffer = app_psram_malloc(WEB_CHUNK_BYTES);
  if (!buffer) return fail(req, "503 Service Unavailable", "内存不足");
  esp_err_t result = httpd_resp_send_chunk(req, (const char *)header, sizeof(header));
  for (size_t offset = 0; result == ESP_OK && offset < bytes;) {
    size_t count = app_barge_diagnostic_read((unsigned)id, track,
                                             offset, buffer, WEB_CHUNK_BYTES);
    if (!count) { result = ESP_FAIL; break; }
    result = httpd_resp_send_chunk(req, (const char *)buffer, count);
    offset += count;
    if (!session_touch()) result = ESP_FAIL;
  }
  free(buffer);
  if (result == ESP_OK) result = httpd_resp_send_chunk(req, NULL, 0);
  return result;
}

static void add_files(cJSON *root, const char *key, bool image,
                      const app_media_snapshot_t *snapshot) {
  cJSON *files = cJSON_AddArrayToObject(root, key);
  unsigned count = image ? snapshot->image_count : snapshot->audio_count;
  for (unsigned i = 0; i < count && i < APP_MEDIA_MAX_FILES; ++i) {
    const char *name = image ? snapshot->images[i] : snapshot->audio[i];
    char path[112];
    snprintf(path, sizeof(path), "/sdcard/media/%s/%s",
             image ? "images" : "audio", name);
    struct stat info;
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "index", i);
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddStringToObject(entry, "label", app_media_file_label(name));
    cJSON_AddBoolToObject(entry, "ambient",
                          !image && app_media_audio_is_ambient(name));
    cJSON_AddNumberToObject(entry, "bytes",
                            stat(path, &info) == 0 ? info.st_size : 0);
    cJSON_AddItemToArray(files, entry);
  }
}

static esp_err_t catalog_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  app_media_snapshot_t *snapshot = app_psram_malloc(sizeof(*snapshot));
  if (!snapshot)
    return fail(req, "503 Service Unavailable", "内存不足");
  app_media_snapshot(snapshot);
  cJSON *root = cJSON_CreateObject();
  if (!root) {
    free(snapshot);
    return fail(req, "503 Service Unavailable", "内存不足");
  }
  add_files(root, "images", true, snapshot);
  add_files(root, "audio", false, snapshot);
  cJSON_AddBoolToObject(root, "playing", snapshot->playing);
  cJSON_AddBoolToObject(root, "paused", snapshot->paused);
  cJSON_AddBoolToObject(root, "repeat", snapshot->repeat);
  cJSON_AddNumberToObject(root, "audio_index", snapshot->audio_index);
  cJSON_AddNumberToObject(root, "wallpaper_index", snapshot->wallpaper_index);
  cJSON_AddNumberToObject(root, "elapsed", snapshot->elapsed_seconds);
  cJSON_AddNumberToObject(root, "duration", snapshot->duration_seconds);
  cJSON_AddStringToObject(root, "message", snapshot->message);
  free(snapshot);
  uint64_t total = 0, free_bytes = 0;
  if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) == ESP_OK) {
    cJSON_AddNumberToObject(root, "total_bytes", (double)total);
    cJSON_AddNumberToObject(root, "free_bytes", (double)free_bytes);
  }
  cJSON_AddNumberToObject(root, "reserve_bytes", APP_MEDIA_RESERVE_BYTES);
  app_media_appearance_t appearance;
  app_media_get_appearance(&appearance);
  cJSON *a = cJSON_AddObjectToObject(root, "appearance");
  cJSON_AddBoolToObject(a, "wallpaper_enabled", appearance.wallpaper_enabled);
  cJSON_AddBoolToObject(a, "night_enabled", appearance.night_enabled);
  cJSON_AddBoolToObject(a, "idle_album", appearance.idle_album);
  cJSON_AddNumberToObject(a, "night_start_minute",
                          appearance.night_start_minute);
  cJSON_AddNumberToObject(a, "night_end_minute", appearance.night_end_minute);
  cJSON_AddNumberToObject(a, "night_brightness", appearance.night_brightness);
  cJSON_AddNumberToObject(a, "idle_seconds", appearance.idle_seconds);
  app_media_anniversary_t anniversaries[APP_MEDIA_ANNIVERSARIES];
  app_media_get_anniversaries(anniversaries);
  cJSON *dates = cJSON_AddArrayToObject(root, "anniversaries");
  for (unsigned i = 0; i < APP_MEDIA_ANNIVERSARIES; ++i) {
    cJSON *date = cJSON_CreateObject();
    cJSON_AddStringToObject(date, "label", anniversaries[i].label);
    cJSON_AddNumberToObject(date, "year", anniversaries[i].year);
    cJSON_AddNumberToObject(date, "month", anniversaries[i].month);
    cJSON_AddNumberToObject(date, "day", anniversaries[i].day);
    cJSON_AddBoolToObject(date, "annual", anniversaries[i].annual);
    cJSON_AddItemToArray(dates, date);
  }
  return json_reply(req, root);
}

static cJSON *receive_json(httpd_req_t *req) {
  if (!req->content_len || req->content_len >= WEB_JSON_BYTES)
    return NULL;
  char body[WEB_JSON_BYTES];
  size_t used = 0;
  int64_t deadline_us = esp_timer_get_time() + 5000000LL;
  while (used < req->content_len) {
    if (!transfer_live(deadline_us))
      return NULL;
    int received = httpd_req_recv(req, body + used, req->content_len - used);
    if (received <= 0)
      return NULL;
    used += (size_t)received;
  }
  body[used] = 0;
  return cJSON_ParseWithLength(body, used + 1);
}

static bool uint_field(const cJSON *root, const char *name, unsigned maximum,
                       unsigned *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsNumber(item) || item->valuedouble < 0 ||
      item->valuedouble > maximum ||
      item->valuedouble != (double)(unsigned)item->valuedouble)
    return false;
  *out = (unsigned)item->valuedouble;
  return true;
}

static bool bool_field(const cJSON *root, const char *name, bool *out) {
  const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, name);
  if (!cJSON_IsBool(item))
    return false;
  *out = cJSON_IsTrue(item);
  return true;
}

static bool within_catalog(unsigned index, bool image) {
  app_media_snapshot_t *snapshot = app_psram_malloc(sizeof(*snapshot));
  if (!snapshot)
    return false;
  app_media_snapshot(snapshot);
  unsigned count = image ? snapshot->image_count : snapshot->audio_count;
  free(snapshot);
  return index < count;
}

static esp_err_t command_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  cJSON *root = receive_json(req);
  if (!root)
    return fail(req, "400 Bad Request", "请求格式或长度有误");
  const cJSON *action = cJSON_GetObjectItemCaseSensitive(root, "action");
  bool ok = false;
  uint32_t job_id = 0;
  if (cJSON_IsString(action)) {
    const char *name = action->valuestring;
    unsigned value = 0;
    if (!strcmp(name, "image")) {
      bool wallpaper;
      ok = uint_field(root, "index", APP_MEDIA_MAX_FILES - 1, &value) &&
           within_catalog(value, true) &&
           bool_field(root, "wallpaper", &wallpaper) &&
           (job_id = app_media_image_tracked(value, wallpaper)) != 0;
    } else if (!strcmp(name, "appearance")) {
      app_media_appearance_t a = {0};
      ok =
          bool_field(root, "wallpaper_enabled", &a.wallpaper_enabled) &&
          bool_field(root, "night_enabled", &a.night_enabled) &&
          bool_field(root, "idle_album", &a.idle_album) &&
          uint_field(root, "night_start_minute", 1439, &a.night_start_minute) &&
          uint_field(root, "night_end_minute", 1439, &a.night_end_minute) &&
          uint_field(root, "night_brightness", 100, &a.night_brightness) &&
          uint_field(root, "idle_seconds", 3600, &a.idle_seconds) &&
          (job_id = app_media_appearance_tracked(&a)) != 0;
    } else if (!strcmp(name, "anniversary")) {
      app_media_anniversary_t date = {0};
      const cJSON *label = cJSON_GetObjectItemCaseSensitive(root, "label");
      ok = cJSON_IsString(label) &&
           strlen(label->valuestring) < sizeof(date.label) &&
           uint_field(root, "index", APP_MEDIA_ANNIVERSARIES - 1, &value);
      if (ok) {
        snprintf(date.label, sizeof(date.label), "%s", label->valuestring);
        ok = !date.label[0] || (uint_field(root, "year", 2099, &date.year) &&
                                uint_field(root, "month", 12, &date.month) &&
                                uint_field(root, "day", 31, &date.day) &&
                                bool_field(root, "annual", &date.annual));
        if (ok)
          ok = (job_id = app_media_anniversary_tracked(value, &date)) != 0;
      }
    } else {
      static const struct {
        const char *name;
        app_media_command_t command;
        unsigned maximum;
      } commands[] = {{"play", APP_MEDIA_PLAY, APP_MEDIA_MAX_FILES - 1},
                      {"pause", APP_MEDIA_PAUSE, 0},
                      {"resume", APP_MEDIA_RESUME, 0},
                      {"stop", APP_MEDIA_STOP, 0},
                      {"next", APP_MEDIA_NEXT, 0},
                      {"previous", APP_MEDIA_PREVIOUS, 0},
                      {"repeat", APP_MEDIA_REPEAT, 1},
                      {"timer", APP_MEDIA_AMBIENT_TIMER, 240}};
      for (unsigned i = 0; i < sizeof(commands) / sizeof(commands[0]); ++i) {
        if (strcmp(name, commands[i].name))
          continue;
        ok = uint_field(root, "value", commands[i].maximum, &value) &&
             (commands[i].command != APP_MEDIA_PLAY ||
              within_catalog(value, false)) &&
             (job_id = app_media_command_tracked(commands[i].command, value)) != 0;
        break;
      }
    }
  }
  cJSON_Delete(root);
  if (!ok)
    return fail(req, "409 Conflict", "参数无效、文件不存在或设备忙");
  cJSON *result = cJSON_CreateObject();
  cJSON_AddBoolToObject(result, "accepted", true);
  cJSON_AddNumberToObject(result, "job_id", job_id);
  httpd_resp_set_status(req, "202 Accepted");
  return json_reply(req, result);
}

static esp_err_t state_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  return json_reply(req, app_control_snapshot());
}
static esp_err_t media_job_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  char query[48], text[16], *end;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", text, sizeof(text)) != ESP_OK ||
      !*text || strspn(text, "0123456789") != strlen(text))
    return fail(req, "400 Bad Request", "操作编号无效");
  errno = 0;
  unsigned long id = strtoul(text, &end, 10);
  if (errno || *end || !id || id > UINT32_MAX)
    return fail(req, "400 Bad Request", "操作编号无效");
  app_media_job_t job;
  if (!app_media_job_status((uint32_t)id, &job))
    return fail(req, "404 Not Found", "媒体操作编号已失效，请读取设备状态确认");
  cJSON *out = cJSON_CreateObject();
  if (out) {
    cJSON_AddNumberToObject(out, "job_id", job.id);
    cJSON_AddBoolToObject(out, "done", job.done);
    cJSON_AddBoolToObject(out, "applied", job.applied);
    cJSON_AddBoolToObject(out, "saved", job.saved);
    cJSON_AddStringToObject(out, "message", job.message);
  }
  return json_reply(req, out);
}

static esp_err_t device_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  cJSON *root = receive_json(req);
  if (!cJSON_IsObject(root)) {
    cJSON_Delete(root);
    return fail(req, "400 Bad Request", "请求无效");
  }
  char *json = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  uint32_t id = json ? app_control_submit(json) : 0;
  free(json);
  if (!id)
    return fail(req, "409 Conflict", "有配置操作正在执行，请等完成后再提交");
  cJSON *out = cJSON_CreateObject();
  if (out) {
    cJSON_AddNumberToObject(out, "job_id", id);
    cJSON_AddStringToObject(out, "status", "pending");
  }
  httpd_resp_set_status(req, "202 Accepted");
  return json_reply(req, out);
}
static esp_err_t job_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  char query[48], id_text[16];
  char *end = NULL;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "id", id_text, sizeof(id_text)) != ESP_OK ||
      !id_text[0] || strspn(id_text, "0123456789") != strlen(id_text))
    return fail(req, "400 Bad Request", "操作编号无效");
  errno = 0;
  unsigned long id = strtoul(id_text, &end, 10);
  if (errno || *end || !id || id > UINT32_MAX)
    return fail(req, "400 Bad Request", "操作编号无效");
  cJSON *out = app_control_status((uint32_t)id);
  if (!out)
    return fail(req, "404 Not Found", "操作编号已失效");
  return json_reply(req, out);
}

// Names are ASCII-only; query values are never decoded into filesystem paths.
static bool file_request(httpd_req_t *req, bool *image, char *name,
                         size_t name_bytes, char *path, size_t path_bytes) {
  char query[128], kind[8];
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
      httpd_query_key_value(query, "kind", kind, sizeof(kind)) != ESP_OK ||
      httpd_query_key_value(query, "name", name, name_bytes) != ESP_OK)
    return false;
  if (strcmp(kind, "image") && strcmp(kind, "audio"))
    return false;
  *image = !strcmp(kind, "image");
  if (!app_media_valid_name(name, *image))
    return false;
  snprintf(path, path_bytes, "/sdcard/media/%s/%s", *image ? "images" : "audio",
           name);
  return true;
}

static esp_err_t download_handler(httpd_req_t *req) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  char name[APP_MEDIA_NAME_BYTES], path[112];
  bool image;
  if (!file_request(req, &image, name, sizeof(name), path, sizeof(path)))
    return fail(req, "400 Bad Request", "文件名无效");
  if (!app_media_validate_file(path, image))
    return fail(req, "404 Not Found", "媒体文件不存在或格式无效");
  FILE *file = fopen(path, "rb");
  if (!file)
    return fail(req, "503 Service Unavailable", "无法读取SD卡");
  struct stat st;
  if (fstat(fileno(file), &st) || st.st_size <= 0) {
    fclose(file);
    return fail(req, "503 Service Unavailable", "无法读取文件长度");
  }
  uint64_t first = 0, last = (uint64_t)st.st_size - 1;
  size_t range_bytes = httpd_req_get_hdr_value_len(req, "Range");
  char range[96], content_range[96];
  if (range_bytes) {
    if (range_bytes >= sizeof(range) ||
        httpd_req_get_hdr_value_str(req, "Range", range, sizeof(range)) != ESP_OK ||
        !app_http_range(range, (uint64_t)st.st_size, &first, &last)) {
      fclose(file);
      snprintf(content_range, sizeof(content_range), "bytes */%llu", (unsigned long long)st.st_size);
      httpd_resp_set_hdr(req, "Content-Range", content_range);
      return fail(req, "416 Range Not Satisfiable", "仅支持有效的单段字节范围");
    }
    if (fseek(file, (long)first, SEEK_SET)) {
      fclose(file);
      return fail(req, "503 Service Unavailable", "无法定位文件");
    }
    snprintf(content_range, sizeof(content_range), "bytes %llu-%llu/%llu",
             (unsigned long long)first, (unsigned long long)last, (unsigned long long)st.st_size);
    httpd_resp_set_status(req, "206 Partial Content");
    httpd_resp_set_hdr(req, "Content-Range", content_range);
  }
  httpd_resp_set_hdr(req, "Accept-Ranges", "bytes");
  httpd_resp_set_type(req, image ? "application/octet-stream" : "audio/wav");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_set_hdr(req, "X-Content-Type-Options", "nosniff");
  unsigned char buffer[WEB_CHUNK_BYTES];
  int64_t deadline_us = esp_timer_get_time() + WEB_TRANSFER_SECONDS * 1000000LL;
  size_t bytes;
  uint64_t remaining = last - first + 1;
  esp_err_t result = ESP_OK;
  while (remaining) {
    size_t wanted = remaining < sizeof(buffer) ? (size_t)remaining : sizeof(buffer);
    bytes = fread(buffer, 1, wanted, file);
    if (!bytes) { result = ESP_FAIL; break; }
    remaining -= bytes;
    if (!transfer_live(deadline_us)) {
      result = ESP_FAIL;
      break;
    }
    result = httpd_resp_send_chunk(req, (const char *)buffer, bytes);
    if (result != ESP_OK)
      break;
    if (!session_touch()) {
      result = ESP_FAIL;
      break;
    }
  }
  if (ferror(file))
    result = ESP_FAIL;
  fclose(file);
  if (result == ESP_OK)
    result = httpd_resp_send_chunk(req, NULL, 0);
  return result;
}

static unsigned file_count(bool image) {
  DIR *directory =
      opendir(image ? "/sdcard/media/images" : "/sdcard/media/audio");
  if (!directory)
    return APP_MEDIA_MAX_FILES;
  unsigned count = 0;
  struct dirent *entry;
  while ((entry = readdir(directory)))
    if (app_media_valid_name(entry->d_name, image))
      ++count;
  closedir(directory);
  return count;
}

static bool stop_selected_audio(const char *name) {
  app_media_snapshot_t *snapshot = app_psram_malloc(sizeof(*snapshot));
  if (!snapshot)
    return false;
  app_media_snapshot(snapshot);
  bool selected = snapshot->audio_index < snapshot->audio_count &&
                  !strcmp(snapshot->audio[snapshot->audio_index], name);
  bool stopped = !selected;
  if (selected && app_media_command(APP_MEDIA_STOP, 0)) {
    int64_t deadline_us = esp_timer_get_time() + 1000000LL;
    do {
      app_media_snapshot(snapshot);
      stopped = !snapshot->playing && !snapshot->paused &&
                snapshot->elapsed_seconds == 0;
      if (stopped)
        break;
      vTaskDelay(pdMS_TO_TICKS(10));
    } while (esp_timer_get_time() < deadline_us);
  }
  free(snapshot);
  return stopped;
}

static esp_err_t upload_file(httpd_req_t *req) {
  char name[APP_MEDIA_NAME_BYTES], path[112], part[120], backup[120];
  bool image;
  if (!file_request(req, &image, name, sizeof(name), path, sizeof(path)))
    return fail(req, "400 Bad Request", "仅接受安全的 .rgb565 或 .wav 文件名");
  if ((image && req->content_len != APP_MEDIA_IMAGE_BYTES) ||
      (!image &&
       (req->content_len < 44 || req->content_len > APP_MEDIA_MAX_WAV_BYTES)))
    return fail(req, "413 Payload Too Large", "图片须240×240，音频最大32MiB");
  snprintf(part, sizeof(part), "%s.part", path);
  snprintf(backup, sizeof(backup), "%s.old", path);
  // Recover an interrupted replacement before beginning the next upload.
  if (access(backup, F_OK) == 0) {
    if (access(path, F_OK) != 0) {
      if (rename(backup, path) != 0)
        return fail(req, "503 Service Unavailable", "旧媒体恢复失败");
    } else if (!app_media_validate_file(path, image)) {
      return fail(req, "503 Service Unavailable",
                  "媒体替换未完成，请检查 .old 备份");
    } else if (unlink(backup) != 0) {
      return fail(req, "503 Service Unavailable", "无法清理旧媒体备份");
    }
  }
  struct stat info;
  bool exists = stat(path, &info) == 0;
  if (!exists && file_count(image) >= APP_MEDIA_MAX_FILES)
    return fail(req, "409 Conflict", "每类最多32个文件，请先删除旧文件");
  // Discard an earlier interrupted transfer while preserving the valid
  // destination.
  if (access(part, F_OK) == 0 && unlink(part) != 0)
    return fail(req, "503 Service Unavailable", "无法清理未完成的上传");
  uint64_t total, free_bytes;
  if (esp_vfs_fat_info("/sdcard", &total, &free_bytes) != ESP_OK ||
      free_bytes < (uint64_t)req->content_len + APP_MEDIA_RESERVE_BYTES)
    return fail(req, "507 Insufficient Storage", "SD空间不足，需保留8MiB");
  FILE *file = fopen(part, "wb");
  if (!file)
    return fail(req, "503 Service Unavailable", "SD写入失败");
  unsigned char buffer[WEB_CHUNK_BYTES];
  size_t remaining = req->content_len;
  bool ok = true;
  int64_t deadline_us = esp_timer_get_time() + WEB_TRANSFER_SECONDS * 1000000LL;
  int64_t stall_deadline_us =
      esp_timer_get_time() + WEB_TRANSFER_STALL_SECONDS * 1000000LL;
  while (remaining) {
    if (!transfer_live(deadline_us) ||
        esp_timer_get_time() >= stall_deadline_us) {
      ok = false;
      break;
    }
    size_t want = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
    int bytes = httpd_req_recv(req, (char *)buffer, want);
    if (bytes == HTTPD_SOCK_ERR_TIMEOUT)
      continue;
    if (bytes <= 0 || !transfer_live(deadline_us) ||
        fwrite(buffer, 1, (size_t)bytes, file) != (size_t)bytes ||
        !session_touch()) {
      ok = false;
      break;
    }
    remaining -= (size_t)bytes;
    stall_deadline_us =
        esp_timer_get_time() + WEB_TRANSFER_STALL_SECONDS * 1000000LL;
  }
  if (fflush(file) != 0 || fsync(fileno(file)) != 0)
    ok = false;
  if (fclose(file) != 0)
    ok = false;
  if (!ok || !app_media_validate_file(part, image)) {
    unlink(part);
    return fail(req, "400 Bad Request",
                "上传不完整或媒体格式无效，原文件未改动");
  }
  if (!image && !stop_selected_audio(name)) {
    unlink(part);
    return fail(req, "503 Service Unavailable", "播放器忙，原文件未改动");
  }
  if (exists && rename(path, backup) != 0) {
    unlink(part);
    return fail(req, "503 Service Unavailable", "无法保留原媒体");
  }
  if (rename(part, path) != 0) {
    if (exists)
      rename(backup, path);
    unlink(part);
    return fail(req, "503 Service Unavailable", "安装失败，原媒体保留在 .old");
  }
  if (exists)
    unlink(backup);
  app_media_rescan();
  return httpd_resp_sendstr(req, "上传完成");
}

static esp_err_t delete_file(httpd_req_t *req) {
  char name[APP_MEDIA_NAME_BYTES], path[112];
  bool image;
  if (!file_request(req, &image, name, sizeof(name), path, sizeof(path)))
    return fail(req, "400 Bad Request", "文件名无效");
  if (!image && !stop_selected_audio(name))
    return fail(req, "503 Service Unavailable", "播放器忙，原文件未改动");
  if (unlink(path) != 0)
    return fail(req, "404 Not Found", "删除失败或文件不存在");
  app_media_rescan();
  return httpd_resp_sendstr(req, "已删除");
}

static esp_err_t mutate_file(httpd_req_t *req, bool upload) {
  if (!session_open())
    return fail(req, "503 Service Unavailable", "网页管理已关闭，请在设备上重新开启");
  bool held = app_media_suspend(1000);
  if (!held) {
    app_media_resume();
    return fail(req, "503 Service Unavailable", "音频忙，请稍后重试");
  }
  esp_err_t result = upload ? upload_file(req) : delete_file(req);
  app_media_resume();
  return result;
}

static esp_err_t upload_handler(httpd_req_t *req) {
  return mutate_file(req, true);
}

static esp_err_t delete_handler(httpd_req_t *req) {
  return mutate_file(req, false);
}

// FAT rename can be interrupted by power loss. Retain a valid .old until
// the installed final file has been validated. Limit work in handoff priority.
static bool recover_directory(bool image, int64_t deadline_us) {
  const char *directory =
      image ? "/sdcard/media/images" : "/sdcard/media/audio";
  DIR *dir = opendir(directory);
  if (!dir)
    return false;
  bool clean = true;
  unsigned scanned = 0, old_files = 0;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (++scanned > 128 || old_files >= APP_MEDIA_MAX_FILES ||
        esp_timer_get_time() >= deadline_us) {
      clean = false;
      break;
    }
    size_t length = strlen(entry->d_name);
    if (length <= 4 || length >= APP_MEDIA_NAME_BYTES + 4 ||
        strcmp(entry->d_name + length - 4, ".old"))
      continue;
    char name[APP_MEDIA_NAME_BYTES];
    memcpy(name, entry->d_name, length - 4);
    name[length - 4] = 0;
    if (!app_media_valid_name(name, image))
      continue;
    ++old_files;
    char path[112], old[120];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    snprintf(old, sizeof(old), "%s.old", path);
    if (!app_media_validate_file(old, image)) {
      clean = false;
      continue;
    }
    struct stat info;
    if (stat(path, &info) != 0 && errno == ENOENT) {
      if (rename(old, path) != 0)
        clean = false;
    } else if (app_media_validate_file(path, image)) {
      if (unlink(old) != 0)
        clean = false;
    } else {
      // Keep both names for manual inspection and preserve the unknown
      // file from removable media.
      clean = false;
    }
  }
  closedir(dir);
  return clean;
}

static esp_err_t start_server(void) {
  app_media_snapshot_t *snapshot = app_psram_malloc(sizeof(*snapshot));
  if (!snapshot)
    return ESP_ERR_NO_MEM;
  app_media_snapshot(snapshot);
  bool ready = snapshot->ready;
  free(snapshot);
  if (!ready || !app_network_is_connected())
    return ESP_ERR_INVALID_STATE;
  bool held = app_media_suspend(1000);
  if (!held) {
    app_media_resume();
    return ESP_ERR_TIMEOUT;
  }
  int64_t recovery_deadline = esp_timer_get_time() + 2000000LL;
  bool recovered_images = recover_directory(true, recovery_deadline);
  bool recovered_audio = recover_directory(false, recovery_deadline);
  app_media_rescan();
  app_media_resume();
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.stack_size = 8192;
  config.max_open_sockets = 2;
  config.max_uri_handlers = 18;
  config.recv_wait_timeout = 3;
  config.send_wait_timeout = 15;
  config.lru_purge_enable = true;
  esp_err_t result = httpd_start(&s_server, &config);
  if (result != ESP_OK)
    return result;
  static const httpd_uri_t handlers[] = {
      {.uri = "/", .method = HTTP_GET, .handler = index_handler},
      {.uri = "/api/device", .method = HTTP_POST, .handler = device_handler},
      {.uri = "/api/job", .method = HTTP_GET, .handler = job_handler},
      {.uri = "/api/state", .method = HTTP_GET, .handler = state_handler},
      {.uri = "/api/media-job", .method = HTTP_GET, .handler = media_job_handler},
      {.uri = "/api/catalog", .method = HTTP_GET, .handler = catalog_handler},
      {.uri = "/api/command", .method = HTTP_POST, .handler = command_handler},
      {.uri = "/api/wake-diagnostic", .method = HTTP_GET, .handler = wake_diagnostic_handler},
      {.uri = "/api/wake-diagnostic", .method = HTTP_POST, .handler = wake_diagnostic_handler},
      {.uri = "/api/wake-diagnostic", .method = HTTP_DELETE, .handler = wake_diagnostic_handler},
      {.uri = "/api/wake-diagnostic/audio", .method = HTTP_GET, .handler = wake_audio_handler},
      {.uri = "/api/barge-diagnostic", .method = HTTP_GET, .handler = barge_diagnostic_handler},
      {.uri = "/api/barge-diagnostic", .method = HTTP_POST, .handler = barge_diagnostic_handler},
      {.uri = "/api/barge-diagnostic", .method = HTTP_DELETE, .handler = barge_diagnostic_handler},
      {.uri = "/api/barge-diagnostic/audio", .method = HTTP_GET, .handler = barge_audio_handler},
      {.uri = "/api/file", .method = HTTP_GET, .handler = download_handler},
      {.uri = "/api/file", .method = HTTP_PUT, .handler = upload_handler},
      {.uri = "/api/file", .method = HTTP_DELETE, .handler = delete_handler}};
  for (unsigned i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i) {
    result = httpd_register_uri_handler(s_server, &handlers[i]);
    if (result != ESP_OK)
      return result;
  }
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip = {0};
  if (!netif || esp_netif_get_ip_info(netif, &ip) != ESP_OK)
    return ESP_ERR_INVALID_STATE;
  portENTER_CRITICAL(&s_status_lock);
  snprintf(s_status.url, sizeof(s_status.url), "http://" IPSTR "/",
           IP2STR(&ip.ip));
  s_status.enabled = true;
  s_expires_us = esp_timer_get_time() + WEB_SESSION_SECONDS * 1000000LL;
  snprintf(s_status.message, sizeof(s_status.message), "%s",
           recovered_images && recovered_audio
               ? "同Wi-Fi访问，空闲10分钟后关闭"
               : "已开启；有未完成的.old媒体，请检查SD卡");
  portEXIT_CRITICAL(&s_status_lock);
  return ESP_OK;
}

static void stop_server(void) {
  portENTER_CRITICAL(&s_status_lock);
  s_status.enabled = false;
  s_expires_us = 0;
  portEXIT_CRITICAL(&s_status_lock);
  if (s_server) {
    httpd_stop(s_server);
    s_server = NULL;
  }
  (void)app_wake_diagnostic_clear();
  (void)app_barge_diagnostic_clear();
}

static void web_worker(void *argument) {
  (void)argument;
  while (true) {
    if (atomic_load(&s_requested) && !s_server) {
      if (start_server() != ESP_OK) {
        stop_server();
        atomic_store(&s_requested, false);
        set_message("未开启：请检查Wi-Fi、SD卡或音频忙状态");
      }
    }
    app_media_web_status_t status;
    app_media_web_status(&status);
    if (s_server && (!atomic_load(&s_requested) || !status.seconds_remaining ||
                     !app_network_is_connected())) {
      stop_server();
      atomic_store(&s_requested, false);
      set_message("网页管理已关闭");
    }
    vTaskDelay(pdMS_TO_TICKS(200));
  }
}

void app_media_web_request(bool enabled) {
  atomic_store(&s_requested, enabled);
}

void app_media_web_init(void) {
  if (xTaskCreate(web_worker, "media_web", 4096, NULL, 2, NULL) != pdPASS)
    set_message("网页管理任务创建失败");
}
