/**
 * @file    debug_uart.h
 * @brief   STM32 调试串口 (USART2: PA2=TX, PA3=RX, 115200 8N1)
 *
 *  用途: 直接接 CH340 → 电脑串口, 看 STM32 自己的运行日志,
 *        不用再靠桥接板转发。
 *
 *  ★ 与主链路完全独立:
 *      USART1 (PA9/PA10) 仍然是"桥接 ↔ STM32"的控制/状态链路,
 *      USART2 只做单向打印, 不参与控制, 不会影响实时性。
 *
 *  接线:
 *      STM32 PA2 (TX)  → CH340 RXD
 *      STM32 GND       → CH340 GND      (共地, 必须)
 *      STM32 PA3 (RX)  → CH340 TXD      (可选, 目前只打印不收)
 */

#ifndef DEBUG_UART_H
#define DEBUG_UART_H

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 USART2 (PA2/PA3), 115200 8N1 */
void dbg_uart_init(void);

/** 打印一行 (自动补 \r\n), 阻塞发送, 建议只打印短行/低频 */
void dbg_line(const char *text);

/** 格式化打印一行 */
void dbg_printf(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* DEBUG_UART_H */
