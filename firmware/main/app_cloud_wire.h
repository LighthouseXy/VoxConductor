#pragma once

// 与平台无关的音频封装/分片处理，供主机边界测试直接使用。
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

static inline void cloud_u32le(uint8_t *p, uint32_t value) {
  for (int i = 0; i < 4; ++i) p[i] = (uint8_t)(value >> (i * 8));
}

static inline void cloud_wav_header(uint8_t wav[44], uint32_t pcm_bytes,
                                     uint32_t rate) {
  memset(wav, 0, 44);
  memcpy(wav, "RIFF", 4); cloud_u32le(wav + 4, 36 + pcm_bytes);
  memcpy(wav + 8, "WAVEfmt ", 8); cloud_u32le(wav + 16, 16);
  wav[20] = 1; wav[22] = 1;
  cloud_u32le(wav + 24, rate); cloud_u32le(wav + 28, rate * 2);
  wav[32] = 2; wav[34] = 16;
  memcpy(wav + 36, "data", 4); cloud_u32le(wav + 40, pcm_bytes);
}

static inline void cloud_audio_slice(uint8_t *dest, const uint8_t wav[44],
                                      const uint8_t *pcm, size_t pos, size_t n) {
  size_t header_bytes = pos < 44 ? 44 - pos : 0;
  if (header_bytes > n) header_bytes = n;
  if (header_bytes) memcpy(dest, wav + pos, header_bytes);
  if (n > header_bytes)
    memcpy(dest + header_bytes, pcm + pos + header_bytes - 44, n - header_bytes);
}

typedef struct {
  size_t used;
  int frame_used;
  int frame_length;
  bool assembling;
  bool continuation;
} cloud_ws_message_t;

// 已过滤控制帧。返回-1=错误，0=待续，1=完整JSON；buffer额外预留1字节。
static inline int cloud_ws_append(cloud_ws_message_t *state, char *buffer,
                                   size_t capacity, uint8_t opcode, bool fin,
                                   int frame_length, int offset,
                                   const char *data, int length) {
  if ((opcode != 1 && opcode != 0) || frame_length < 0 || offset < 0 ||
      length < 0 || (length && !data)) return -1;
  if (offset == 0) {
    if (opcode == 1) {
      if (state->assembling) return -1;
      state->assembling = true;
      state->used = 0;
    } else if (!state->assembling || !state->continuation) return -1;
    state->frame_used = 0;
    state->frame_length = frame_length;
    state->continuation = false;
  }
  if (!state->assembling || offset != state->frame_used ||
      frame_length != state->frame_length ||
      length > state->frame_length - state->frame_used ||
      (size_t)length > capacity - state->used) return -1;
  if (length) memcpy(buffer + state->used, data, length);
  state->used += length;
  state->frame_used += length;
  if (state->frame_used == state->frame_length) {
    if (fin) {
      buffer[state->used] = '\0';
      state->assembling = false;
      return 1;
    }
    state->continuation = true;
  }
  return 0;
}
