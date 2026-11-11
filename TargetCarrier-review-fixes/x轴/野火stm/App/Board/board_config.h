#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H
/* Wildfire dual USB schematic: V1.2 on cover, V1.1 in sheet footer. */
#define BOARD_NAME "Wildfire STM32F103C8T6 dual USB"
#define BOARD_HSE_HZ 8000000U
#define BOARD_ROBOT_UART_INSTANCE USART3
#define BOARD_ROBOT_UART_HANDLE huart3
/* LED1/2/3: PA1/2/3, active low. Keys: PA0/PC13, active high. */
#endif
