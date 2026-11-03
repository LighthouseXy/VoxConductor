"""Execute the real NVS-load branch with synthetic preference blobs on the host."""
import re
import subprocess
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / 'firmware/main/app_desktop.c').read_text()
layouts = source.split('// Preserve historical NVS layout, without active game state.', 1)[1].split('static QueueHandle_t s_commands;', 1)[0]
loader = source.split('void app_desktop_init(void) {', 1)[1].split('  if (s_preferences.focus.running)', 1)[0]
focus = re.search(r'static bool focus_valid\(.*?\n}', source, re.S).group(0)
program = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "app_focus.h"
#include "app_words.h"
#include "app_tts_options.h"
typedef int nvs_handle_t;
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_NVS_NOT_FOUND, NVS_READONLY };
static bool s_preferences_writable = true, saved;
static unsigned s_home_style, s_wallpaper_mode;
static app_words_t s_words;
static uint32_t word_mask = 5, word_cursor = 7;
static void save_later(void) { saved = true; }
''' + layouts + r'''
static uint8_t blob[sizeof(preferences_t)];
static size_t blob_size;
static int nvs_flash_init(void) { return ESP_OK; }
static int nvs_open(const char *name, int mode, nvs_handle_t *h) {
  assert(!strcmp(name, "desktop") && mode == NVS_READONLY); *h = 1; return ESP_OK;
}
static int nvs_get_blob(nvs_handle_t h, const char *key, void *out, size_t *size) {
  assert(h == 1 && !strcmp(key, "preferences"));
  if (!blob_size) return ESP_ERR_NVS_NOT_FOUND;
  if (out) { assert(*size >= blob_size); memcpy(out, blob, blob_size); }
  *size = blob_size; return ESP_OK;
}
static int nvs_get_u32(nvs_handle_t h, const char *key, uint32_t *out) {
  if (!strcmp(key, "words_mask")) { *out = word_mask; return ESP_OK; }
  if (!strcmp(key, "words_cursor")) { *out = word_cursor; return ESP_OK; }
  assert(h == 1); *out = !strcmp(key, "home_style") ? 1 : 4; return ESP_OK;
}
static void nvs_close(nvs_handle_t h) { assert(h == 1); }
''' + focus + '\nstatic void load(void) {\n' + loader + r'''
}
static void reset(void) {
  s_preferences = (preferences_t){.version=5, .volume=35, .brightness=100,
      .tts_rate_percent=100, .focus={.work_minutes=25, .break_minutes=5}};
  saved=false; s_preferences_writable=true;
  s_words = (app_words_t){0};
}
int main(void) {
  typedef struct { unsigned version, volume, reserved_legacy[18];
      bool wake_enabled; app_focus_t focus; unsigned brightness; } previous_t;
  assert(sizeof(preferences_t) == sizeof(previous_t));
  for (unsigned version=3; version<=4; ++version) {
    previous_t old={.version=version,.volume=27,.brightness=69,.wake_enabled=true,
        .focus={.work_minutes=40,.break_minutes=8}};
    memset(old.reserved_legacy, 0xff, sizeof(old.reserved_legacy));
    memcpy(blob,&old,sizeof(old)); blob_size=sizeof(old); reset(); load();
    assert(saved && s_preferences_writable && s_preferences.version==5);
    assert(s_preferences.tts_rate_percent==100 && s_preferences.tts_voice_id==0);
    assert(s_preferences.volume==27 && s_preferences.brightness==69 && s_preferences.wake_enabled);
    assert(s_preferences.focus.work_minutes==40 && s_home_style==1 && s_wallpaper_mode==4);
  }
  for (unsigned version=1; version<=2; ++version) {
    legacy_preferences_t old={.version=version,.volume=23,.wake_enabled=true,
        .focus={.work_minutes=35,.break_minutes=7}};
    memcpy(blob,&old,sizeof(old)); blob_size=sizeof(old); reset(); load();
    assert(saved && s_preferences_writable && s_preferences.version==5);
    assert(s_preferences.volume==23 && s_preferences.tts_rate_percent==100 && s_preferences.tts_voice_id==0);
  }
  reset(); s_preferences.tts_rate_percent=125; s_preferences.tts_voice_id=3;
  memcpy(blob,&s_preferences,sizeof(s_preferences)); blob_size=sizeof(s_preferences); reset(); load();
  assert(!saved && s_preferences_writable && s_preferences.tts_rate_percent==125 && s_preferences.tts_voice_id==3);
  preferences_t corrupt; memcpy(&corrupt,blob,sizeof(corrupt));
  corrupt.tts_rate_percent=101; memcpy(blob,&corrupt,sizeof(corrupt)); reset(); load();
  assert(!saved && !s_preferences_writable && s_preferences.tts_rate_percent==100);
  blob_size=0; reset(); load();
  assert(!saved && s_preferences_writable && s_preferences.tts_rate_percent==100);
  assert(s_words.mastered == 5 && s_words.cursor == 7);
  word_mask = UINT32_MAX; word_cursor = 99; reset(); load();
  assert(s_words.mastered == ((1u << APP_WORD_COUNT) - 1) && s_words.cursor == 0);
  puts("PASS: NVS v1-v4 migration, unchanged blob size, persisted v5 reload, invalid/missing settings");
}
'''
import tempfile
with tempfile.TemporaryDirectory(prefix='vox-tts-nvs-') as tmp:
    output = str(Path(tmp) / 'test')
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-I' + str(root / 'firmware/main'), '-x', 'c', '-', '-o', output],
                   input=program, text=True, check=True)
    subprocess.run([output], check=True)
