#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
void app_acoustic_init(void);
// Allocate DSP after ASR, without accepting noise during answer generation.
bool app_acoustic_prepare(void);
// Playback owner arms only when actual audio starts; coordinator disarms on exit.
// Disarming preserves an already captured interruption until take().
// false reports DSP arming failure; expose that failure to the coordinator.
bool app_acoustic_listen(bool active);
// Explicit button/session exit: invalidate pending voice, never auto-resume it.
void app_acoustic_discard(void);
void app_acoustic_playing(bool active);
// Reference is a 16 kHz copy of the post-volume 24 kHz speaker signal.
void app_acoustic_reference(const int16_t *pcm, size_t count);
// Called exclusively by the microphone reader, at 16 kHz.
void app_acoustic_feed(const int16_t *pcm, size_t count);
// WakeNet worker posts a recognized 小冰小冰; stale playback epochs are rejected.
bool app_acoustic_wake_detected(unsigned epoch);
bool app_acoustic_pending(void);
// Caller holds the microphone mutex; stops pending capture and consumes it.
size_t app_acoustic_take(int16_t *pcm, size_t capacity, size_t *silence_samples);
