#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { int dummy; } TIM_HandleTypeDef;
typedef struct { int dummy; } UART_HandleTypeDef;
#define GPIOA ((void *)1)
#define BOARD_LED_R_Pin 2U
#define BOARD_LED_G_Pin 4U
#define BOARD_LED_B_Pin 8U
#define NVIC_PRIORITYGROUP_4 3U
unsigned HAL_NVIC_GetPriorityGrouping(void);
void HAL_GPIO_TogglePin(void *, unsigned);
#define __disable_irq() ((void)0)
#define __NOP() ((void)0)
