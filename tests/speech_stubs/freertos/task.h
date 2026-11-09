#pragma once
#include "FreeRTOS.h"
void vTaskDelay(TickType_t ticks);
void vTaskDelete(void *task);
UBaseType_t uxTaskPriorityGet(void *task);
void vTaskPrioritySet(void *task, UBaseType_t priority);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *arg, unsigned priority, void *handle);
