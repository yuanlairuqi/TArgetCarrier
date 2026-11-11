#ifndef APP_RTOS_H
#define APP_RTOS_H
#include "main.h"
void AppRTOS_Start(TIM_HandleTypeDef *timer, UART_HandleTypeDef *uart);
void AppRTOS_Fatal(const char *file, int line);
extern const char * volatile g_rtos_fault_file;
extern volatile int g_rtos_fault_line;
#endif
