#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define APP_WAKE_PHRASE "小冰小冰"
void app_wake_init(void (*callback)(void));
void app_wake_feed(const int16_t *pcm, size_t count);
// Same model/worker, but echo-cleaned playback input with an acoustic epoch.
void app_wake_feed_barge(const int16_t *pcm, size_t count, unsigned epoch);
void app_wake_allow_barge(bool active);
void app_wake_allow(bool idle);
void app_wake_set_enabled(bool enabled);
// Logical switch; retain an accepted wake during its model rebuild.
bool app_wake_is_enabled(void);
// Runtime readiness, temporarily false during a worker-owned model rebuild.
bool app_wake_available(void);
// True only when the idle microphone stream can actually reach WakeNet.
bool app_wake_input_allowed(void);
