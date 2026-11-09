#pragma once
#include <stdbool.h>
#include <stddef.h>

typedef struct {
  unsigned rate_percent, voice_id;
} app_tts_options_t;

// Stable IDs are saved in NVS; 0 preserves the firmware's configured voice.
static const struct { const char *voice, *label; } app_tts_voices[] = {
    {"", "固件默认"}, {"Cherry", "芊悦 · 自然女声"},
    {"Serena", "苏瑶 · 温柔女声"}, {"Ethan", "晨煦 · 温暖男声"},
    {"Chelsie", "千雪 · 活泼女声"}, {"Moon", "月白 · 率性男声"},
    {"Kai", "凯 · 舒缓男声"}};
#define APP_TTS_VOICE_COUNT (sizeof(app_tts_voices) / sizeof(app_tts_voices[0]))

static inline bool app_tts_options_valid(app_tts_options_t options) {
  return options.rate_percent >= 50 && options.rate_percent <= 200 &&
         options.rate_percent % 5 == 0 && options.voice_id < APP_TTS_VOICE_COUNT;
}

// One short UI-locked snapshot per synthesis session, never per audio frame.
bool app_desktop_tts_options(app_tts_options_t *out);
const char *app_cloud_tts_voice(unsigned id);
