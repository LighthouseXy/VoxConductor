#pragma once
#include <stdint.h>
typedef uint32_t EventBits_t;
typedef struct test_events *EventGroupHandle_t;
#define BIT0 1u
#define BIT1 2u
#define BIT2 4u
#define pdFALSE 0
EventGroupHandle_t xEventGroupCreate(void);
EventBits_t xEventGroupSetBits(EventGroupHandle_t events, EventBits_t bits);
EventBits_t xEventGroupWaitBits(EventGroupHandle_t events, EventBits_t bits, int clear, int all, unsigned wait);
void vEventGroupDelete(EventGroupHandle_t events);
