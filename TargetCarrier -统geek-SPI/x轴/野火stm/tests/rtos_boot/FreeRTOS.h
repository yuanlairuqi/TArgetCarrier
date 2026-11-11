#pragma once
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef uint32_t StackType_t,TickType_t;
typedef struct {int x;} StaticTask_t;
typedef void *TaskHandle_t;
#define configMINIMAL_STACK_SIZE 128U
#define configSTACK_DEPTH_TYPE unsigned
#define pdMS_TO_TICKS(x) (x)
#define configASSERT(x) assert(x)
