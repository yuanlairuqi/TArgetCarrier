#pragma once
#include "FreeRTOS.h"
TaskHandle_t xTaskCreateStatic(void (*)(void *),const char *,unsigned,void *,unsigned,StackType_t *,StaticTask_t *);
TickType_t xTaskGetTickCount(void);
int xTaskDelayUntil(TickType_t *,TickType_t);
void vTaskStartScheduler(void);
