#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APP_MEDIA_MAX_FILES 32
#define APP_MEDIA_NAME_BYTES 49
#define APP_MEDIA_IMAGE_BYTES (240u * 240u * 2u)
#define APP_MEDIA_MAX_WAV_BYTES (32u * 1024u * 1024u)
#define APP_MEDIA_RESERVE_BYTES (8u * 1024u * 1024u)
#define APP_MEDIA_ANNIVERSARIES 4

typedef struct {
  bool wallpaper_enabled, night_enabled, idle_album;
  unsigned night_start_minute, night_end_minute, night_brightness;
  unsigned idle_seconds;
} app_media_appearance_t;
typedef struct {
  char label[48];
  unsigned year, month, day;
  bool annual;
} app_media_anniversary_t;
typedef struct {
  bool ready, playing, paused, repeat, ambient;
  unsigned image_count, audio_count, wallpaper_index, album_index, audio_index;
  unsigned elapsed_seconds, duration_seconds, stop_remaining_seconds;
  uint32_t catalog_generation;
  char images[APP_MEDIA_MAX_FILES][APP_MEDIA_NAME_BYTES];
  char audio[APP_MEDIA_MAX_FILES][APP_MEDIA_NAME_BYTES];
  char message[96];
} app_media_snapshot_t;
typedef enum {
  APP_MEDIA_PLAY,
  APP_MEDIA_PAUSE,
  APP_MEDIA_RESUME,
  APP_MEDIA_STOP,
  APP_MEDIA_NEXT,
  APP_MEDIA_PREVIOUS,
  APP_MEDIA_REPEAT,
  APP_MEDIA_AMBIENT_TIMER
} app_media_command_t;

// One worker owns SD media reads and speaker playback. UI entries never do I/O.
esp_err_t app_media_init(bool sd_ready, void (*audio_activity)(bool));
void app_media_snapshot(app_media_snapshot_t *out);
bool app_media_command(app_media_command_t command, unsigned value);
bool app_media_select_image(unsigned index, bool wallpaper);
void app_media_rescan(void);
// Call outside LVGL. Suspend is counted and closes the music file/clears DMA
// before returning true; every call (including timeout) needs one resume.
bool app_media_suspend(unsigned timeout_ms);
void app_media_resume(void);
// UI copies into its own PSRAM buffer; worker buffers are never given to LVGL.
bool app_media_copy_image(bool album, void *out, size_t bytes,
                          uint32_t *generation);
void app_media_get_appearance(app_media_appearance_t *out);
bool app_media_set_appearance(const app_media_appearance_t *value);
void app_media_get_anniversaries(
    app_media_anniversary_t out[APP_MEDIA_ANNIVERSARIES]);
bool app_media_set_anniversary(unsigned index,
                               const app_media_anniversary_t *value);
// Empty label deletes. days is next annual occurrence or signed one-shot days.
bool app_media_anniversary_days(const app_media_anniversary_t *item,
                                long long utc_now, int *days);
bool app_media_valid_name(const char *name, bool image);
bool app_media_validate_file(const char *path, bool image);
// env_*.wav is ambience; all other WAV files are music. Unknown titles retain
// their filename, so uploaded files do not need a metadata database.
bool app_media_audio_is_ambient(const char *name);
const char *app_media_file_label(const char *name);
typedef struct {
  uint32_t id;
  bool done, applied, saved;
  char message[96];
} app_media_job_t;
// Four bounded receipts; IDs identify a request, not a filename/index guess.
uint32_t app_media_command_tracked(app_media_command_t command, unsigned value);
uint32_t app_media_image_tracked(unsigned index, bool wallpaper);
uint32_t app_media_appearance_tracked(const app_media_appearance_t *value);
uint32_t app_media_anniversary_tracked(unsigned index, const app_media_anniversary_t *value);
bool app_media_job_status(uint32_t id, app_media_job_t *out);
