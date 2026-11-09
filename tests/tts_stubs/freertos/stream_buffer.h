#pragma once
#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
typedef struct {
  uint8_t *storage; size_t capacity, bytes;
  pthread_mutex_t lock;
  pthread_cond_t changed;
} StaticStreamBuffer_t;
typedef StaticStreamBuffer_t *StreamBufferHandle_t;
StreamBufferHandle_t xStreamBufferCreateStatic(size_t capacity, size_t trigger, uint8_t *storage, StaticStreamBuffer_t *control);
size_t xStreamBufferSend(StreamBufferHandle_t buffer, const void *data, size_t size, unsigned wait);
size_t xStreamBufferReceive(StreamBufferHandle_t buffer, void *data, size_t size, unsigned wait);
size_t xStreamBufferBytesAvailable(StreamBufferHandle_t buffer);
size_t xStreamBufferSpacesAvailable(StreamBufferHandle_t buffer);
void vStreamBufferDelete(StreamBufferHandle_t buffer);
