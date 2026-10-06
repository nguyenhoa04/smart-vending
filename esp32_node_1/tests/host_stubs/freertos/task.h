#pragma once
#include "FreeRTOS.h"
TickType_t xTaskGetTickCount(void);
void vTaskDelay(TickType_t ticks);
int xTaskCreate(void (*task)(void *), const char *name, unsigned stack,
                void *context, unsigned priority, void *handle);
