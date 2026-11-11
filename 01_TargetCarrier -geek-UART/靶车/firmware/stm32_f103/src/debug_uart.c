/**
 * @file    debug_uart.c
 * @brief   STM32 调试串口 USART2 (PA2=TX, PA3=RX), 115200 8N1
 *
 *  纯寄存器实现, 不依赖 HAL 的串口驱动:
 *    - 只做轮询发送, 不开中断 / 不用 DMA (日志量很小, 每 500 ms 一行);
 *    - 在 app_loop (主循环) 里调用, 不进入 1 kHz 控制中断, 不影响控制节拍。
 *
 *  ★ 为什么单独占一个串口:
 *    USART1 已经被桥接链路占用 (PA9/PA10), 而且那一路必须保持 921600 的实时性,
 *    不适合再塞调试打印。USART2 完全独立, 断线/插拔都不影响控制。
 */

#include "debug_uart.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "stm32f1xx.h"

/* 36 MHz APB1, 115200 -> BRR = 36000000 / 115200 ≈ 312.5 -> 0x138
 * 用整数近似: 312 = 0x138, 误差 ~0.16%, 串口能正常收。 */
#define DBG_BRR     312u

void dbg_uart_init(void)
{
    /* 1. 时钟: GPIOA + USART2 (APB1) + AFIO 不需要 */
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART2EN;
    (void)RCC->APB1ENR;      /* 读一次, 保证时钟生效 */

    /* 2. PA2 = 复用推挽输出 50MHz (CNF=10, MODE=11 -> 0xB)
     *    PA3 = 上拉/下拉输入   (CNF=10, MODE=00 -> 0x8)
     *    CRL 里 PA2 是 bit11:8, PA3 是 bit15:12 */
    uint32_t crl = GPIOA->CRL;
    crl &= ~(0xFu << 8);
    crl |= (0xBu << 8);          /* PA2: AF push-pull 50MHz */
    crl &= ~(0xFu << 12);
    crl |= (0x8u << 12);         /* PA3: input with pull */
    GPIOA->CRL = crl;
    GPIOA->ODR |= (1u << 3);     /* PA3 上拉 */

    /* 3. USART2: 115200 8N1, 收发都打开 (RX 留着以后用) */
    USART2->CR1 = 0;
    USART2->CR2 = 0;
    USART2->CR3 = 0;
    USART2->BRR = DBG_BRR;
    USART2->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
}

void dbg_line(const char *text)
{
    if (text == NULL) {
        return;
    }
    for (const char *p = text; *p != '\0'; p++) {
        while ((USART2->SR & USART_SR_TXE) == 0) {
            /* 等发送寄存器空 */
        }
        USART2->DR = (uint8_t)(*p);
    }
    while ((USART2->SR & USART_SR_TXE) == 0) {
    }
    while ((USART2->SR & USART_SR_TC) == 0) {
    }
    USART2->DR = '\r';
    while ((USART2->SR & USART_SR_TXE) == 0) {
    }
    USART2->DR = '\n';
    while ((USART2->SR & USART_SR_TC) == 0) {
    }
}

void dbg_printf(const char *fmt, ...)
{
    char buf[176];   /* 诊断行很长, 96 会被截断 */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    dbg_line(buf);
}
