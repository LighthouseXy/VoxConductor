/*
 * 媒体核心：有界目录、SD读取、音乐/环境音播放和偏好持久化。
 * 工作任务拥有文件/播放状态，UI只提交命令/复制快照，不直接读SD。
 * 语音等通过计数式挂起让出音频，每次挂起（含超时）需对应恢复。
 */
#include "esp_attr.h"
#include "app_media.h"
#include "app_audio.h"
#include "app_memory_alloc.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include <dirent.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define IMAGE_DIR "/sdcard/media/images"
#define AUDIO_DIR "/sdcard/media/audio"
#define WAV_RATE 24000u
#define PCM_SAMPLES 512u
#define PREFERENCE_VERSION 1u

typedef struct {
  unsigned version;
  app_media_appearance_t appearance;
  app_media_anniversary_t anniversaries[APP_MEDIA_ANNIVERSARIES];
  char wallpaper[APP_MEDIA_NAME_BYTES];
  bool repeat;
} media_preferences_t;
typedef struct {
  uint32_t offset, bytes;
} wav_info_t;
enum {
  REQUEST_AUDIO,
  REQUEST_IMAGE,
  REQUEST_SCAN,
  REQUEST_APPEARANCE,
  REQUEST_ANNIVERSARY
};
typedef struct {
  unsigned type, index;
  uint32_t job_id;
  bool wallpaper;
  app_media_command_t command;
  union {
    app_media_appearance_t appearance;
    app_media_anniversary_t anniversary;
    char image_name[APP_MEDIA_NAME_BYTES];
  } value;
} media_request_t;

static const char *TAG = "app_media";
static SemaphoreHandle_t s_lock;
static QueueHandle_t s_requests;
static EXT_RAM_BSS_ATTR app_media_snapshot_t s_view;
static media_preferences_t s_preferences = {
    .version = PREFERENCE_VERSION,
    .appearance = {.night_start_minute = 22 * 60,
                   .night_end_minute = 7 * 60,
                   .night_brightness = 10,
                   .idle_seconds = 60}};
static void *s_images[2];
static uint32_t s_image_generation[2];
static _Atomic unsigned s_holds;
static _Atomic bool s_busy;
static _Atomic bool s_worker_ready;
static void (*s_audio_activity)(bool);
// Only the media worker uses these playback/SD variables.
static FILE *s_track;
static wav_info_t s_wav;
static uint32_t s_position;
static int16_t s_last_sample;
static int16_t *s_pcm_buffer;
static size_t s_pcm_count, s_pcm_cursor;
static unsigned s_track_index;
static bool s_want_play, s_user_paused, s_started, s_release_failed, s_sd_ready,
    s_scan_pending;
static int64_t s_stop_at;
static bool s_preferences_writable = true;
static bool s_image_pending;
static media_request_t s_pending_image;
static app_media_job_t s_jobs[4];
static uint32_t s_job_sequence;
static bool s_last_persist_ok;
static void lock(void) { xSemaphoreTake(s_lock, portMAX_DELAY); }
static void unlock(void) { xSemaphoreGive(s_lock); }
static void complete_job(uint32_t id, bool applied, bool saved) {
  if (!id || !s_lock) return;
  lock();
  app_media_job_t *job = &s_jobs[id % 4];
  if (job->id == id) {
    job->done = true;
    job->applied = applied;
    job->saved = saved;
    snprintf(job->message, sizeof(job->message), "%s", s_view.message);
  }
  unlock();
}

static void message(const char *text) {
  if (!s_lock)
    return;
  lock();
  snprintf(s_view.message, sizeof(s_view.message), "%s", text);
  unlock();
}
static uint16_t le16(const unsigned char *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static uint32_t le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static bool wav_read(FILE *file, wav_info_t *out) {
  if (fseek(file, 0, SEEK_END))
    return false;
  long size = ftell(file);
  if (size < 44 || (unsigned long)size > APP_MEDIA_MAX_WAV_BYTES ||
      fseek(file, 0, SEEK_SET))
    return false;
  unsigned char header[16];
  if (fread(header, 1, 12, file) != 12 || memcmp(header, "RIFF", 4) ||
      memcmp(header + 8, "WAVE", 4))
    return false;
  uint64_t end = (uint64_t)le32(header + 4) + 8;
  if (end < 12 || end != (uint64_t)size)
    return false;
  bool format = false, data = false;
  wav_info_t info = {0};
  // Real PCM files need only a few chunks. Bound hostile zero-length chunk
  // work.
  unsigned chunks = 0;
  for (uint64_t pos = 12; pos + 8 <= end && chunks++ < 256;) {
    if (fseek(file, (long)pos, SEEK_SET) || fread(header, 1, 8, file) != 8)
      return false;
    uint32_t bytes = le32(header + 4);
    uint64_t next = pos + 8 + bytes + (bytes & 1u);
    if (next > end)
      return false;
    if (!memcmp(header, "fmt ", 4)) {
      if (format || bytes < 16 || fread(header, 1, 16, file) != 16 ||
          le16(header) != 1 || le16(header + 2) != 1 ||
          le32(header + 4) != WAV_RATE || le32(header + 8) != WAV_RATE * 2 ||
          le16(header + 12) != 2 || le16(header + 14) != 16)
        return false;
      format = true;
    } else if (!memcmp(header, "data", 4)) {
      if (data || !bytes || (bytes & 1u))
        return false;
      info.offset = (uint32_t)(pos + 8);
      info.bytes = bytes;
      data = true;
    }
    pos = next;
    if (pos == end) {
      if (format && data) {
        *out = info;
        return true;
      }
      return false;
    }
  }
  return false;
}

bool app_media_valid_name(const char *name, bool image) {
  if (!name)
    return false;
  size_t length = strnlen(name, APP_MEDIA_NAME_BYTES);
  const char *extension = image ? ".rgb565" : ".wav";
  size_t suffix = strlen(extension);
  if (length <= suffix || length >= APP_MEDIA_NAME_BYTES || name[0] == '.' ||
      strstr(name, "..") || strcmp(name + length - suffix, extension))
    return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)name[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
          (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'))
      return false;
  }
  return true;
}
bool app_media_audio_is_ambient(const char *name) {
  return name && !strncmp(name, "env_", 4);
}
const char *app_media_file_label(const char *name) {
  if(!strcmp(name,"wall_05_lunar.rgb565"))return "悬浮月环";
  if(!strcmp(name,"wall_06_jelly.rgb565"))return "流光水母";
  static const struct {
    const char *file, *label;
  } labels[] = {{"music_01_carefree.wav", "无忧时光"},
                {"music_02_easy_lemon.wav", "清新柠檬"},
                {"music_03_dream_culture.wav", "梦境钢琴"},
                {"music_04_evening.wav", "宁静傍晚"},
                {"music_05_gymnopedie_1.wav", "吉姆诺佩蒂一"},
                {"music_06_gymnopedie_2.wav", "吉姆诺佩蒂二"},
                {"music_07_gymnopedie_3.wav", "吉姆诺佩蒂三"},
                {"music_08_meditation_1.wav", "冥想即兴一"},
                {"music_09_meditation_2.wav", "冥想即兴二"},
                {"music_10_meditation_3.wav", "冥想即兴三"},
                {"env_01_rain.wav", "细雨"},
                {"env_02_ocean.wav", "海浪"},
                {"env_03_wind.wav", "轻风"},
                {"env_04_stream.wav", "溪流"},
                {"env_05_fire.wav", "炉火"},
                {"env_06_white.wav", "白噪音"},
                {"env_07_pink.wav", "粉红噪音"},
                {"env_08_brown.wav", "棕噪音"},
                {"wall_01_jade.rgb565", "青山薄雾"},
                {"wall_02_aurora.rgb565", "极光湖面"},
                {"wall_03_orbit.rgb565", "星际轨道"},
                {"wall_04_dunes.rgb565", "暖色沙丘"}};
  if (!name)
    return "";
  for (unsigned i = 0; i < sizeof(labels) / sizeof(*labels); ++i)
    if (!strcmp(name, labels[i].file))
      return labels[i].label;
  return name;
}
bool app_media_validate_file(const char *path, bool image) {
  if (!path)
    return false;
  struct stat status;
  if (stat(path, &status) || !S_ISREG(status.st_mode))
    return false;
  if (image)
    return status.st_size == APP_MEDIA_IMAGE_BYTES;
  FILE *file = fopen(path, "rb");
  if (!file)
    return false;
  wav_info_t info;
  bool valid = wav_read(file, &info);
  fclose(file);
  return valid;
}

static bool leap(unsigned year) {
  return !(year % 4) && ((year % 100) || !(year % 400));
}
static bool valid_date(unsigned year, unsigned month, unsigned day) {
  static const unsigned days[] = {31, 28, 31, 30, 31, 30,
                                  31, 31, 30, 31, 30, 31};
  return year >= 1970 && year <= 2199 && month >= 1 && month <= 12 &&
         day >= 1 && day <= days[month - 1] + (month == 2 && leap(year));
}
static long long civil_days(unsigned year, unsigned month, unsigned day) {
  int y = (int)year - (month <= 2);
  int era = y / 400, yo = y - era * 400;
  int mp = (int)month + (month > 2 ? -3 : 9);
  int doy = (153 * mp + 2) / 5 + (int)day - 1;
  return (long long)era * 146097 + yo * 365 + yo / 4 - yo / 100 + doy - 719468;
}
static bool valid_label(const char *label, size_t size) {
  size_t length = strnlen(label, size);
  if (length == size)
    return false;
  for (size_t i = 0; i < length;) {
    unsigned char c = (unsigned char)label[i++];
    if (c < 32 || c == 127)
      return false;
    if (c < 128)
      continue;
    unsigned extra;
    uint32_t code;
    if (c >= 0xc2 && c <= 0xdf) {
      extra = 1;
      code = c & 31;
    } else if (c >= 0xe0 && c <= 0xef) {
      extra = 2;
      code = c & 15;
    } else if (c >= 0xf0 && c <= 0xf4) {
      extra = 3;
      code = c & 7;
    } else
      return false;
    if (i + extra > length)
      return false;
    for (unsigned j = 0; j < extra; ++j) {
      c = (unsigned char)label[i++];
      if ((c & 0xc0) != 0x80)
        return false;
      code = (code << 6) | (c & 63);
    }
    if ((extra == 1 && code < 128) || (extra == 2 && code < 2048) ||
        (extra == 3 && code < 65536) || code > 0x10ffff ||
        (code >= 0xd800 && code <= 0xdfff))
      return false;
  }
  return true;
}
static bool valid_anniversary(const app_media_anniversary_t *item) {
  return item && valid_label(item->label, sizeof(item->label)) &&
         (!item->label[0] || valid_date(item->year, item->month, item->day));
}
bool app_media_anniversary_days(const app_media_anniversary_t *item,
                                long long utc_now, int *days) {
  if (!days || !valid_anniversary(item) || !item->label[0] ||
      utc_now < 1704067200LL || utc_now > 7258118400LL)
    return false;
  time_t local = (time_t)(utc_now + 8 * 3600);
  struct tm now;
  if (!gmtime_r(&local, &now))
    return false;
  long long today = civil_days((unsigned)now.tm_year + 1900,
                               (unsigned)now.tm_mon + 1, (unsigned)now.tm_mday);
  unsigned year = item->year;
  if (item->annual) {
    year = (unsigned)now.tm_year + 1900;
    while (year <= 2199 && (!valid_date(year, item->month, item->day) ||
                            civil_days(year, item->month, item->day) < today))
      ++year;
    if (year > 2199)
      return false;
  }
  *days = (int)(civil_days(year, item->month, item->day) - today);
  return true;
}
static bool valid_appearance(const app_media_appearance_t *value) {
  return value && value->night_start_minute < 1440 &&
         value->night_end_minute < 1440 && value->night_brightness <= 100 &&
         value->idle_seconds <= 3600 &&
         (value->idle_seconds == 0 || value->idle_seconds >= 10);
}
static bool valid_preferences(const media_preferences_t *value) {
  if (value->version != PREFERENCE_VERSION ||
      !valid_appearance(&value->appearance) ||
      (value->wallpaper[0] && !app_media_valid_name(value->wallpaper, true)))
    return false;
  for (unsigned i = 0; i < APP_MEDIA_ANNIVERSARIES; ++i)
    if (!valid_anniversary(&value->anniversaries[i]))
      return false;
  return true;
}
static bool save_preferences(void) {
  s_last_persist_ok = false;
  if (!s_preferences_writable) {
    message("配置版本不支持，未保存");
    return false;
  }
  nvs_handle_t handle;
  esp_err_t result = nvs_open("media", NVS_READWRITE, &handle);
  if (result == ESP_OK) {
    result =
        nvs_set_blob(handle, "settings", &s_preferences, sizeof(s_preferences));
    if (result == ESP_OK)
      result = nvs_commit(handle);
    nvs_close(handle);
  }
  if (result != ESP_OK) {
    ESP_LOGW(TAG, "媒体设置保存失败：%s", esp_err_to_name(result));
    message("设置已应用，但保存失败");
  }
  s_last_persist_ok = result == ESP_OK;
  return s_last_persist_ok;
}

void app_media_snapshot(app_media_snapshot_t *out) {
  if (!out)
    return;
  if (!s_lock) {
    memset(out, 0, sizeof(*out));
    return;
  }
  lock();
  *out = s_view;
  unlock();
}
void app_media_get_appearance(app_media_appearance_t *out) {
  if (!out)
    return;
  if (!s_lock) {
    *out = s_preferences.appearance;
    return;
  }
  lock();
  *out = s_preferences.appearance;
  unlock();
}
void app_media_get_anniversaries(
    app_media_anniversary_t out[APP_MEDIA_ANNIVERSARIES]) {
  if (!out)
    return;
  if (!s_lock) {
    memset(out, 0, sizeof(s_preferences.anniversaries));
    return;
  }
  lock();
  memcpy(out, s_preferences.anniversaries, sizeof(s_preferences.anniversaries));
  unlock();
}
static bool enqueue(const media_request_t *request) {
  return atomic_load(&s_worker_ready) && s_requests &&
         xQueueSend(s_requests, request, 0) == pdTRUE;
}
static uint32_t enqueue_tracked(media_request_t *request) {
  if (!s_lock || !atomic_load(&s_worker_ready) || !s_requests) return 0;
  lock();
  uint32_t id = s_job_sequence + 1;
  if (!id) id = 1;
  app_media_job_t *job = &s_jobs[id % 4];
  if (job->id && !job->done) { unlock(); return 0; }
  s_job_sequence = request->job_id = id;
  *job = (app_media_job_t){.id = id};
  snprintf(job->message, sizeof(job->message), "请求已入队，尚未执行");
  if (request->type == REQUEST_AUDIO && request->command == APP_MEDIA_PLAY) {
    if (request->index >= s_view.audio_count) { job->done = true; unlock(); return 0; }
    snprintf(request->value.image_name, sizeof(request->value.image_name), "%s",
             s_view.audio[request->index]);
  }
  unlock();
  if (!enqueue(request)) { complete_job(id, false, false); return 0; }
  return id;
}
bool app_media_job_status(uint32_t id, app_media_job_t *out) {
  if (!s_lock || !id || !out) return false;
  lock();
  bool found = s_jobs[id % 4].id == id;
  if (found) *out = s_jobs[id % 4];
  unlock();
  return found;
}
uint32_t app_media_command_tracked(app_media_command_t command, unsigned value) {
  if (command > APP_MEDIA_AMBIENT_TIMER ||
      (command == APP_MEDIA_AMBIENT_TIMER && value > 240)) return 0;
  media_request_t request = {.type = REQUEST_AUDIO, .command = command, .index = value};
  return enqueue_tracked(&request);
}
uint32_t app_media_image_tracked(unsigned index, bool wallpaper) {
  if (!s_lock) return 0;
  media_request_t request = {.type = REQUEST_IMAGE, .index = index, .wallpaper = wallpaper};
  lock();
  if (index >= s_view.image_count) { unlock(); return 0; }
  snprintf(request.value.image_name, sizeof(request.value.image_name), "%s", s_view.images[index]);
  unlock();
  return enqueue_tracked(&request);
}
uint32_t app_media_appearance_tracked(const app_media_appearance_t *value) {
  if (!valid_appearance(value)) return 0;
  media_request_t request = {.type = REQUEST_APPEARANCE};
  request.value.appearance = *value;
  return enqueue_tracked(&request);
}
uint32_t app_media_anniversary_tracked(unsigned index, const app_media_anniversary_t *value) {
  if (index >= APP_MEDIA_ANNIVERSARIES || !valid_anniversary(value)) return 0;
  media_request_t request = {.type = REQUEST_ANNIVERSARY, .index = index};
  request.value.anniversary = *value;
  return enqueue_tracked(&request);
}
bool app_media_command(app_media_command_t command, unsigned value) {
  if (command > APP_MEDIA_AMBIENT_TIMER ||
      (command == APP_MEDIA_AMBIENT_TIMER && value > 240))
    return false;
  media_request_t request = {
      .type = REQUEST_AUDIO, .index = value, .command = command};
  return enqueue(&request);
}
bool app_media_select_image(unsigned index, bool wallpaper) {
  if (!s_lock)
    return false;
  media_request_t request = {
      .type = REQUEST_IMAGE, .index = index, .wallpaper = wallpaper};
  lock();
  if (index >= s_view.image_count) {
    unlock();
    return false;
  }
  snprintf(request.value.image_name, sizeof(request.value.image_name), "%s",
           s_view.images[index]);
  unlock();
  return enqueue(&request);
}
void app_media_rescan(void) {
  const media_request_t request = {.type = REQUEST_SCAN};
  if (!enqueue(&request))
    message("媒体队列已满，请稍后刷新");
}
bool app_media_set_appearance(const app_media_appearance_t *value) {
  if (!valid_appearance(value))
    return false;
  media_request_t request = {.type = REQUEST_APPEARANCE};
  request.value.appearance = *value;
  return enqueue(&request);
}
bool app_media_set_anniversary(unsigned index,
                               const app_media_anniversary_t *value) {
  if (index >= APP_MEDIA_ANNIVERSARIES || !valid_anniversary(value))
    return false;
  media_request_t request = {.type = REQUEST_ANNIVERSARY, .index = index};
  request.value.anniversary = *value;
  return enqueue(&request);
}
bool app_media_copy_image(bool album, void *out, size_t bytes,
                          uint32_t *generation) {
  if (!s_lock || !out || bytes < APP_MEDIA_IMAGE_BYTES || !generation)
    return false;
  unsigned slot = album ? 1 : 0;
  lock();
  bool changed = s_images[slot] && *generation != s_image_generation[slot];
  if (changed) {
    memcpy(out, s_images[slot], APP_MEDIA_IMAGE_BYTES);
    *generation = s_image_generation[slot];
  }
  unlock();
  return changed;
}
bool app_media_suspend(unsigned timeout_ms) {
  atomic_fetch_add(&s_holds, 1);
  int64_t end = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
  while (atomic_load(&s_busy)) {
    if (esp_timer_get_time() >= end)
      return false;
    vTaskDelay(pdMS_TO_TICKS(5) ? pdMS_TO_TICKS(5) : 1);
  }
  return true;
}
void app_media_resume(void) {
  unsigned count = atomic_load(&s_holds);
  while (count && !atomic_compare_exchange_weak(&s_holds, &count, count - 1)) {
  }
}

static bool begin_io(void) {
  if (s_release_failed)
    return false;
  atomic_store(&s_busy, true);
  if (!atomic_load(&s_holds))
    return true;
  atomic_store(&s_busy, false);
  return false;
}
static int name_compare(const void *a, const void *b) { return strcmp(a, b); }
static unsigned
scan_directory(const char *directory, bool image,
               char names[APP_MEDIA_MAX_FILES][APP_MEDIA_NAME_BYTES]) {
  DIR *dir = opendir(directory);
  if (!dir)
    return 0;
  unsigned count = 0;
  struct dirent *entry;
  while ((entry = readdir(dir))) {
    if (atomic_load(&s_holds))
      break;
    if (!app_media_valid_name(entry->d_name, image))
      continue;
    char path[128];
    int path_length = snprintf(path, sizeof(path), "%s/%.*s", directory,
                               APP_MEDIA_NAME_BYTES - 1, entry->d_name);
    if (path_length < 0 || (size_t)path_length >= sizeof(path))
      continue;
    if (!app_media_validate_file(path, image))
      continue;
    // Keep the alphabetically first 32, independent of FAT directory order.
    unsigned index =
        count < APP_MEDIA_MAX_FILES ? count++ : APP_MEDIA_MAX_FILES - 1;
    if (count == APP_MEDIA_MAX_FILES &&
        strcmp(entry->d_name, names[index]) >= 0 && names[index][0])
      continue;
    snprintf(names[index], APP_MEDIA_NAME_BYTES, "%.*s",
             APP_MEDIA_NAME_BYTES - 1, entry->d_name);
    qsort(names, count, APP_MEDIA_NAME_BYTES, name_compare);
    if (atomic_load(&s_holds))
      break;
  }
  closedir(dir);
  return count;
}
static unsigned find_name(char names[APP_MEDIA_MAX_FILES][APP_MEDIA_NAME_BYTES],
                          unsigned count, const char *name) {
  for (unsigned i = 0; i < count; ++i)
    if (!strcmp(names[i], name))
      return i;
  return 0;
}
static bool load_image(unsigned index, bool wallpaper, bool activate) {
  unsigned slot = wallpaper ? 0 : 1;
  char name[APP_MEDIA_NAME_BYTES];
  lock();
  if (index >= s_view.image_count) {
    unlock();
    message("没有这张图片");
    return false;
  }
  snprintf(name, sizeof(name), "%s", s_view.images[index]);
  unlock();
  if (!begin_io())
    return false;
  void *pixels = app_psram_malloc(APP_MEDIA_IMAGE_BYTES);
  char path[128];
  snprintf(path, sizeof(path), IMAGE_DIR "/%s", name);
  FILE *file = pixels ? fopen(path, "rb") : NULL;
  bool read =
      file &&
      fread(pixels, 1, APP_MEDIA_IMAGE_BYTES, file) == APP_MEDIA_IMAGE_BYTES &&
      fgetc(file) == EOF && !ferror(file);
  if (file)
    fclose(file);
  if (read) {
    // Collapse dim background remnants to true RGB565 black. Subject pixels
    // remain unchanged; physical LCD leakage cannot be fixed by image data.
    uint8_t *raw=pixels;
    for(size_t i=0;i<APP_MEDIA_IMAGE_BYTES;i+=2){
      unsigned p=raw[i]|((unsigned)raw[i+1]<<8);
      unsigned r=((p>>11)&31)*255/31,g=((p>>5)&63)*255/63,b=(p&31)*255/31;
      if(r<32 && g<32 && b<32)raw[i]=raw[i+1]=0;
    }
    lock();
    void *old = s_images[slot];
    s_images[slot] = pixels;
    if (++s_image_generation[slot] == 0)
      ++s_image_generation[slot];
    if (wallpaper) {
      s_view.wallpaper_index = index;
      if (activate)
        s_preferences.appearance.wallpaper_enabled = true;
      snprintf(s_preferences.wallpaper, sizeof(s_preferences.wallpaper), "%s",
               name);
    } else
      s_view.album_index = index;
    unlock();
    free(old);
  } else {
    free(pixels);
    message("图片读取失败或内存不足");
  }
  atomic_store(&s_busy, false);
  if (read && wallpaper)
    save_preferences();
  return read;
}
static void scan_media(void) {
  if (!s_sd_ready || !begin_io())
    return;
  app_media_snapshot_t *catalog = app_psram_zero(sizeof(*catalog));
  if (!catalog) {
    atomic_store(&s_busy, false);
    s_scan_pending = false;
    message("媒体目录内存不足");
    return;
  }
  catalog->image_count = scan_directory(IMAGE_DIR, true, catalog->images);
  catalog->audio_count = scan_directory(AUDIO_DIR, false, catalog->audio);
  bool interrupted = atomic_load(&s_holds) != 0;
  if (!interrupted) {
    lock();
    char track_name[APP_MEDIA_NAME_BYTES] = {0};
    if (s_track_index < s_view.audio_count)
      snprintf(track_name, sizeof(track_name), "%s",
               s_view.audio[s_track_index]);
    memcpy(s_view.images, catalog->images, sizeof(s_view.images));
    memcpy(s_view.audio, catalog->audio, sizeof(s_view.audio));
    s_view.image_count = catalog->image_count;
    s_view.audio_count = catalog->audio_count;
    s_view.wallpaper_index =
        find_name(s_view.images, s_view.image_count, s_preferences.wallpaper);
    s_view.album_index =
        s_view.album_index < s_view.image_count ? s_view.album_index : 0;
    s_track_index = find_name(s_view.audio, s_view.audio_count, track_name);
    if (track_name[0] && (!s_view.audio_count ||
                          strcmp(track_name, s_view.audio[s_track_index]))) {
      // A renamed/deleted song must never resume another song at its old
      // offset.
      s_want_play = s_user_paused = false;
      s_position = 0;
      memset(&s_wav, 0, sizeof(s_wav));
      snprintf(s_view.message, sizeof(s_view.message),
               "原音频已移除，播放停止");
    }
    s_view.audio_index = s_track_index;
    if (++s_view.catalog_generation == 0)
      ++s_view.catalog_generation;
    unlock();
    s_scan_pending = false;
  }
  free(catalog);
  atomic_store(&s_busy, false);
  if (!interrupted) {
    lock();
    unsigned count = s_view.image_count, selected = s_view.wallpaper_index;
    unsigned album = s_view.album_index;
    unlock();
    if (count) {
      load_image(selected, true, false);
      load_image(album, false, false);
    } else {
      lock();
      for (unsigned i = 0; i < 2; ++i) {
        free(s_images[i]);
        s_images[i] = NULL;
        if (++s_image_generation[i] == 0)
          ++s_image_generation[i];
      }
      unlock();
    }
  }
}

static void publish_playback(void) {
  lock();
  s_view.playing = s_track != NULL;
  s_view.paused = s_user_paused;
  if (s_want_play && atomic_load(&s_holds))
    s_view.paused = true;
  s_view.audio_index = s_track_index;
  s_view.elapsed_seconds = s_position / (WAV_RATE * 2);
  s_view.duration_seconds = s_wav.bytes / (WAV_RATE * 2);
  int64_t remaining = s_stop_at - esp_timer_get_time();
  s_view.stop_remaining_seconds =
      s_stop_at && remaining > 0 ? (unsigned)((remaining + 999999) / 1000000)
                                 : 0;
  s_view.ambient = s_track_index < s_view.audio_count &&
                   app_media_audio_is_ambient(s_view.audio[s_track_index]);
  unlock();
}
static unsigned adjacent_track(bool previous, bool wrap) {
  lock();
  unsigned count = s_view.audio_count, index = s_track_index;
  bool ambient =
      index < count && app_media_audio_is_ambient(s_view.audio[index]);
  for (unsigned step = 0; step < count; ++step) {
    if (!wrap && ((!previous && index + 1 >= count) || (previous && !index)))
      break;
    index = (index + count + (previous ? -1 : 1)) % count;
    if (app_media_audio_is_ambient(s_view.audio[index]) == ambient) {
      unlock();
      return index;
    }
  }
  unlock();
  return count;
}
static void close_track(bool smooth) {
  bool had_track = s_track != NULL;
  if (!had_track && !s_started)
    return;
  if (had_track) {
    fclose(s_track);
    s_track = NULL;
    s_pcm_cursor = s_pcm_count = 0;
  }
  if (s_started) {
    if (smooth && had_track) {
      int16_t tail[256];
      for (unsigned i = 0; i < 128; ++i)
        tail[i] = (int16_t)((int32_t)s_last_sample * (127 - i) / 128);
      memset(tail + 128, 0, sizeof(tail) - 128 * sizeof(*tail));
      if (app_audio_play_media_pcm(tail, 256) == ESP_OK) {
        memset(tail, 0, sizeof(tail));
        // More than the 640-frame DMA depth drains the last sample before
        // clear.
        for (unsigned i = 0; i < 4; ++i)
          if (app_audio_play_media_pcm(tail, 256) != ESP_OK)
            break;
      }
    }
    esp_err_t released = app_audio_abort_playback();
    if (released != ESP_OK) {
      // Keep the speaker lease until DMA disable/clear really succeeds.
      s_release_failed = true;
      message("扬声器释放失败，正在重试");
      publish_playback();
      return;
    }
    s_release_failed = false;
    s_started = false;
    if (s_audio_activity)
      s_audio_activity(false);
  }
  atomic_store(&s_busy, false);
  publish_playback();
}
static bool open_track(void) {
  char name[APP_MEDIA_NAME_BYTES];
  lock();
  bool exists = s_track_index < s_view.audio_count;
  if (exists)
    snprintf(name, sizeof(name), "%s", s_view.audio[s_track_index]);
  unlock();
  if (!exists || !s_pcm_buffer) {
    s_want_play = s_user_paused = false;
    message(exists ? "音频缓冲内存不足" : "SD中没有可播放的WAV");
    return false;
  }
  if (!begin_io())
    return false;
  char path[128];
  snprintf(path, sizeof(path), AUDIO_DIR "/%s", name);
  s_track = fopen(path, "rb");
  if (!s_track || !wav_read(s_track, &s_wav) || s_position >= s_wav.bytes ||
      fseek(s_track, (long)(s_wav.offset + s_position), SEEK_SET)) {
    if (s_track)
      fclose(s_track);
    s_track = NULL;
    s_want_play = s_user_paused = false;
    s_position = 0;
    atomic_store(&s_busy, false);
    message("WAV读取失败，请检查SD文件");
    return false;
  }
  if (atomic_load(&s_holds)) {
    close_track(false);
    return false;
  }
  if (s_audio_activity)
    s_audio_activity(true);
  s_started = true;
  s_last_sample = 0;
  s_pcm_cursor = s_pcm_count = 0;
  publish_playback();
  return true;
}
static bool audio_request(const media_request_t *request) {
  unsigned count;
  lock();
  count = s_view.audio_count;
  unlock();
  if ((request->command == APP_MEDIA_PLAY && (!s_sd_ready || request->index >= count)) ||
      ((request->command == APP_MEDIA_RESUME || request->command == APP_MEDIA_NEXT ||
        request->command == APP_MEDIA_PREVIOUS || request->command == APP_MEDIA_REPEAT) && !count)) {
    message("没有可用音频，操作未完成");
    return false;
  }
  switch (request->command) {
  case APP_MEDIA_PLAY:
    if (!s_sd_ready || request->index >= count) {
      message("没有这首音频");
      break;
    }
    close_track(true);
    s_track_index = request->index;
    lock();
    s_view.repeat = app_media_audio_is_ambient(s_view.audio[s_track_index]) ||
                    s_preferences.repeat;
    unlock();
    s_position = 0;
    memset(&s_wav, 0, sizeof(s_wav));
    s_want_play = true;
    s_user_paused = false;
    message("正在播放");
    break;
  case APP_MEDIA_PAUSE:
    s_want_play = false;
    s_user_paused = count != 0;
    close_track(true);
    message("已暂停");
    break;
  case APP_MEDIA_RESUME:
    if (!s_sd_ready || !count) {
      message("SD中没有可播放的WAV");
      break;
    }
    s_want_play = true;
    s_user_paused = false;
    message("继续播放");
    break;
  case APP_MEDIA_STOP:
    s_want_play = s_user_paused = false;
    close_track(true);
    s_position = 0;
    s_stop_at = 0;
    message("已停止");
    break;
  case APP_MEDIA_NEXT:
  case APP_MEDIA_PREVIOUS:
    if (!count) {
      message("SD中没有可播放的WAV");
      break;
    }
    close_track(true);
    s_track_index =
        adjacent_track(request->command == APP_MEDIA_PREVIOUS, true);
    s_position = 0;
    memset(&s_wav, 0, sizeof(s_wav));
    message(s_want_play ? "正在播放" : "已选择音频");
    break;
  case APP_MEDIA_REPEAT:
    // Explicit settings may pause briefly; flash commits never run under PCM.
    close_track(true);
    lock();
    s_view.repeat = request->index != 0;
    if (!app_media_audio_is_ambient(s_view.audio[s_track_index]))
      s_preferences.repeat = s_view.repeat;
    unlock();
    (void)save_preferences();
    break;
  case APP_MEDIA_AMBIENT_TIMER:
    s_stop_at = request->index
                    ? esp_timer_get_time() + (int64_t)request->index * 60000000
                    : 0;
    message(request->index ? "定时停止已设置" : "定时停止已关闭");
    break;
  }
  publish_playback();
  return !s_release_failed;
}
static void worker(void *unused) {
  (void)unused;
  int16_t pcm[PCM_SAMPLES];
  bool fade_in = true;
  for (;;) {
    if (s_release_failed) {
      close_track(false);
      if (s_release_failed) {
        vTaskDelay(pdMS_TO_TICKS(100));
        continue;
      }
    }
    media_request_t request;
    // Bound command work, so HTTP cannot starve PCM forever.
    for (unsigned n = 0;
         n < 8 && xQueueReceive(s_requests, &request, 0) == pdTRUE; ++n) {
      if (request.type == REQUEST_AUDIO) {
        bool exists = true;
        if (request.job_id && request.command == APP_MEDIA_PLAY) {
          lock();
          request.index = find_name(s_view.audio, s_view.audio_count, request.value.image_name);
          exists = s_view.audio_count && !strcmp(s_view.audio[request.index], request.value.image_name);
          unlock();
        }
        bool applied = exists && audio_request(&request);
        if (!exists) message("所选音频已移除");
        complete_job(request.job_id, applied,
                     applied && (request.command != APP_MEDIA_REPEAT || s_last_persist_ok));
      }
      else if (request.type == REQUEST_SCAN) {
        close_track(true);
        s_scan_pending = true;
      } else if (request.type == REQUEST_IMAGE) {
        // Latest selection wins, without blocking STOP behind a foreground
        // hold.
        if (s_image_pending) complete_job(s_pending_image.job_id, false, false);
        s_pending_image = request;
        s_image_pending = true;
      } else if (request.type == REQUEST_APPEARANCE) {
        close_track(true);
        lock();
        s_preferences.appearance = request.value.appearance;
        unlock();
        bool saved = save_preferences();
        complete_job(request.job_id, true, saved);
      } else if (request.type == REQUEST_ANNIVERSARY) {
        close_track(true);
        lock();
        s_preferences.anniversaries[request.index] = request.value.anniversary;
        unlock();
        bool saved = save_preferences();
        complete_job(request.job_id, true, saved);
      }
      if (s_release_failed)
        break;
    }
    if (s_release_failed)
      continue;
    if (s_stop_at && esp_timer_get_time() >= s_stop_at) {
      s_want_play = s_user_paused = false;
      close_track(true);
      s_position = 0;
      s_stop_at = 0;
      message("定时播放已结束");
    }
    if (atomic_load(&s_holds)) {
      close_track(true);
      fade_in = true;
      publish_playback();
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    if (s_scan_pending) {
      close_track(true);
      scan_media();
      fade_in = true;
    }
    if (s_image_pending) {
      close_track(true);
      if (s_release_failed)
        continue;
      lock();
      unsigned index = find_name(s_view.images, s_view.image_count,
                                 s_pending_image.value.image_name);
      bool exists =
          s_view.image_count &&
          !strcmp(s_view.images[index], s_pending_image.value.image_name);
      unlock();
      if (!exists) {
        s_image_pending = false;
        message("图片已移除");
        complete_job(s_pending_image.job_id, false, false);
      } else {
        bool loaded = load_image(index, s_pending_image.wallpaper, true);
        if (loaded || !atomic_load(&s_holds)) {
          s_image_pending = false;
          complete_job(s_pending_image.job_id, loaded,
                       loaded && (!s_pending_image.wallpaper || s_last_persist_ok));
        }
      }
      fade_in = true;
    }
    if (!s_want_play) {
      publish_playback();
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    if (!s_track) {
      fade_in = true;
      if (!open_track()) {
        vTaskDelay(pdMS_TO_TICKS(20));
        continue;
      }
    }
    if (atomic_load(&s_holds))
      continue;
    lock();
    bool seamless_loop = s_view.ambient && s_view.repeat;
    unlock();
    size_t samples = (s_wav.bytes - s_position) / 2;
    if (samples > PCM_SAMPLES)
      samples = PCM_SAMPLES;
    if (s_pcm_cursor == s_pcm_count) {
      s_pcm_count = (s_wav.bytes - s_position) / 2;
      if (s_pcm_count > 4096)
        s_pcm_count = 4096;
      s_pcm_cursor = 0;
      if (fread(s_pcm_buffer, sizeof(*pcm), s_pcm_count, s_track) !=
          s_pcm_count) {
        s_want_play = s_user_paused = false;
        close_track(false);
        message("SD音频读取中断");
        continue;
      }
    }
    if (samples > s_pcm_count - s_pcm_cursor)
      samples = s_pcm_count - s_pcm_cursor;
    memcpy(pcm, s_pcm_buffer + s_pcm_cursor, samples * sizeof(*pcm));
    for (size_t i = 0; i < samples; ++i) {
      if (fade_in && i < 256)
        pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)i / 256);
      size_t remaining = (s_wav.bytes - s_position) / 2 - i;
      if (!seamless_loop && remaining <= 256)
        pcm[i] = (int16_t)((int32_t)pcm[i] * (int32_t)(remaining - 1) / 256);
    }
    fade_in = false;
    esp_err_t result = app_audio_play_media_pcm(pcm, samples);
    if (result != ESP_OK) {
      s_want_play = s_user_paused = false;
      close_track(false);
      ESP_LOGW(TAG, "本地音频播放失败：%s", esp_err_to_name(result));
      message("音频输出失败");
      continue;
    }
    s_pcm_cursor += samples;
    s_position += (uint32_t)(samples * 2);
    s_last_sample = pcm[samples - 1];
    publish_playback();
    if (s_position == s_wav.bytes) {
      // Periodic ambience rewinds the open WAV without draining/resetting DMA.
      // Stop, pause and voice/reminder holds are still checked next iteration.
      if (seamless_loop && !atomic_load(&s_holds) &&
          !fseek(s_track, (long)s_wav.offset, SEEK_SET)) {
        s_position = 0;
        s_pcm_cursor = s_pcm_count = 0;
        continue;
      }
      close_track(true);
      s_position = 0;
      lock();
      unsigned count = s_view.audio_count;
      bool repeat = s_view.repeat;
      unlock();
      if (repeat && count) {
        // Repeat means this song, so a rain loop never advances into music.
      } else {
        unsigned next = adjacent_track(false, false);
        if (next < count)
          s_track_index = next;
        else {
          s_want_play = false;
          message("播放完毕");
        }
      }
    }
  }
}

esp_err_t app_media_init(bool sd_ready, void (*audio_activity)(bool)) {
  if (s_lock)
    return ESP_ERR_INVALID_STATE;
  s_lock = xSemaphoreCreateMutex();
  s_requests = xQueueCreate(12, sizeof(media_request_t));
  if (!s_lock || !s_requests) {
    if (s_requests)
      vQueueDelete(s_requests);
    s_requests = NULL;
    // The UI may already read the cache. Keep a created mutex valid on failure.
    message("媒体缓存初始化失败");
    return ESP_ERR_NO_MEM;
  }
  s_audio_activity = audio_activity;
  s_sd_ready = sd_ready;
  nvs_handle_t handle;
  if (nvs_open("media", NVS_READONLY, &handle) == ESP_OK) {
    media_preferences_t value;
    size_t bytes = sizeof(value);
    esp_err_t result = nvs_get_blob(handle, "settings", &value, &bytes);
    if (result == ESP_OK && bytes == sizeof(value) &&
        valid_preferences(&value)) {
      lock();
      s_preferences = value;
      unlock();
    } else if (result != ESP_ERR_NVS_NOT_FOUND) {
      // Preserve an unknown/corrupt future blob for recovery.
      s_preferences_writable = false;
      message("媒体配置未识别，使用默认值");
    }
    nvs_close(handle);
  }
  lock();
  s_view.repeat = s_preferences.repeat;
  s_view.ready = sd_ready;
  unlock();
  if (sd_ready)
    s_pcm_buffer = app_psram_malloc(4096 * sizeof(*s_pcm_buffer));
  if (sd_ready) {
    const char *dirs[] = {"/sdcard/media", IMAGE_DIR, AUDIO_DIR};
    for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); ++i) {
      if (mkdir(dirs[i], 0775) && errno != EEXIST) {
        s_sd_ready = false;
        lock();
        s_view.ready = false;
        unlock();
        message("媒体目录创建失败");
        break;
      }
    }
  } else
    message("SD未挂载，媒体不可用");
  s_scan_pending = s_sd_ready;
  if (xTaskCreate(worker, "media", 6144, NULL, 4, NULL) != pdPASS) {
    free(s_pcm_buffer);
    s_pcm_buffer = NULL;
    vQueueDelete(s_requests);
    s_requests = NULL;
    lock();
    s_view.ready = false;
    unlock();
    message("媒体任务启动失败");
    return ESP_ERR_NO_MEM;
  }
  atomic_store(&s_worker_ready, true);
  return ESP_OK;
}
