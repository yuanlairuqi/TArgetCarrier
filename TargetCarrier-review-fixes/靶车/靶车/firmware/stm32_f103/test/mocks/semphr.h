#include "FreeRTOS.h"
static inline SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *s) {return s;}
static inline int xSemaphoreTake(SemaphoreHandle_t s,TickType_t t) {(void)t;return s!=NULL;}
static inline int xSemaphoreGive(SemaphoreHandle_t s) {return s!=NULL;}
