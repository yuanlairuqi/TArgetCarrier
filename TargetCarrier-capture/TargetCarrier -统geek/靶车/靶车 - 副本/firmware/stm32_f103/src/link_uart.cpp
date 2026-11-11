/**
 * @file    link_uart.cpp
 * @brief   STM32 <-> M5Stamp 桥接端 USART1 链路的 FreeRTOS 适配层
 *
 *  ★ 为什么这个文件还是 .cpp:
 *    STM32duino 的 Serial1 是 C++ 对象; 纯 C 文件包含 Arduino 头会报
 *    "unknown type name 'namespace'"。本文件把 C++ 依赖关在驱动层,
 *    对外只暴露 C 接口, 业务逻辑 (app_main.c) 保持纯 C。
 *
 *  ★ 接收为什么用 DMA 而不是 Serial1.read():
 *    软件 I2C 刷一屏 OLED、串口日志都会占住任务; 921600 下每 19us 一个字节,
 *    一旦停止搬运就会丢帧。DMA 循环搬运 + 任务消费, 让"慢任务"只影响延迟,
 *    不影响字节完整性; 同时用"消费滞后检测"保证旧字节不会被当成新命令。
 *
 *  ★ 发送为什么只有 status 任务调用:
 *    单一写者 -> 不需要互斥量, 不会有半帧交错; 17 字节 @921600 约 184us,
 *    20~70 帧/秒的发送量远低于串口带宽。
 */

#include <Arduino.h>
#include <HardwareSerial.h>

#include "link_uart.h"
#include "config.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* DMA 循环接收                                                       */
/* ------------------------------------------------------------------ */

static uint8_t  s_rx_dma[LINK_UART_RX_CAP];
static uint16_t s_rx_read = 0;        /* 读指针; 写指针由 CNDTR 反推 */
static volatile bool s_overflow = false;
static uint32_t s_rx_bytes = 0;
static uint32_t s_overflow_count = 0;
static uint32_t s_tx_fail = 0;
static uint32_t s_last_consume_us = 0;
static bool s_have_consume_time = false;

/* 消费滞后阈值: 读指针落后写指针超过这个量, 说明环形缓冲可能已经被
 * DMA 覆盖过, 数据不再可信。此时宁可丢一段重新找帧头, 也不能把
 * "断联前/半截的旧命令"当成新命令执行。 */
#define LINK_RX_LAG_LIMIT (LINK_UART_RX_CAP - 64u)   /* 448 B */

static void link_uart_rx_dma_start(void)
{
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;

    DMA1_Channel5->CCR  &= ~DMA_CCR_EN;
    DMA1_Channel5->CPAR  = (uint32_t)&USART1->DR;
    DMA1_Channel5->CMAR  = (uint32_t)s_rx_dma;
    DMA1_Channel5->CNDTR = LINK_UART_RX_CAP;
    DMA1_Channel5->CCR   = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_PL;

    USART1->CR1 &= ~USART_CR1_RXNEIE;   /* 关掉 Arduino 的逐字节中断 */
    USART1->CR3 |= USART_CR3_DMAR;
    DMA1_Channel5->CCR |= DMA_CCR_EN;

    s_rx_read = 0;
}

static void link_uart_rx_dma_ensure(void)
{
    bool dead = ((DMA1_Channel5->CCR & DMA_CCR_EN) == 0u) ||
                (DMA1_Channel5->CNDTR == 0u) ||
                (DMA1_Channel5->CNDTR > LINK_UART_RX_CAP);
    if (!dead) {
        return;
    }
    link_uart_rx_dma_start();
    s_overflow = true;      /* DMA 重启过: 半帧/旧字节不可信 */
}

static uint16_t link_uart_write_index(void)
{
    uint32_t cnt = DMA1_Channel5->CNDTR;
    if (cnt > LINK_UART_RX_CAP) {
        cnt = LINK_UART_RX_CAP;
    }
    return (uint16_t)(LINK_UART_RX_CAP - cnt);
}

extern "C" void link_uart_init(void)
{
    /* Serial1 = USART1 (PA9=TX / PA10=RX), 波特率与桥接端一致 */
    Serial1.begin(LINK_BAUD, SERIAL_8N1);
    Serial1.setTimeout(0);
    link_uart_rx_dma_start();   /* 之后接收走 DMA */
}

extern "C" uint16_t link_uart_read(uint8_t *out, uint16_t max)
{
    if (out == NULL || max == 0u) {
        return 0;
    }

    link_uart_rx_dma_ensure();

    uint32_t stamp = micros();
    bool late = s_have_consume_time && (uint32_t)(stamp - s_last_consume_us) >=
        (uint32_t)((LINK_UART_RX_CAP * 10000000ULL) / LINK_BAUD);
    s_last_consume_us = stamp;
    s_have_consume_time = true;
    uint16_t wr = (uint16_t)(link_uart_write_index() % LINK_UART_RX_CAP);
    uint16_t rd = s_rx_read;
    /* A modulo pointer alone cannot distinguish empty from one/multiple full
     * DMA laps. If consumption was delayed by a full ring's wire time, drop
     * the uncertain bytes, even when wr == rd, instead of re-dating old frames. */
    if (late) {
        s_rx_read = wr;
        s_overflow = true;
        s_overflow_count++;
        return 0;
    }

    /* 写指针/读指针都在 [0, CAP)。环形距离:
     * 正常情况 0 <= (wr - rd) <= 已积压字节数。
     * 如果距离异常大 (超过 LAG_LIMIT), 说明读指针被 DMA 追上了:
     * 已读位置之后的数据可能已被覆盖, 直接跳到写指针重新同步。 */
    uint16_t pending = (uint16_t)((wr >= rd) ? (wr - rd) : (wr + LINK_UART_RX_CAP - rd));

    if (pending > LINK_RX_LAG_LIMIT) {
        s_rx_read = wr;
        s_overflow = true;
        s_overflow_count++;
        return 0;
    }

    uint16_t n = 0;
    while (n < max && rd != wr) {
        out[n++] = s_rx_dma[rd];
        rd = (uint16_t)((rd + 1u) % LINK_UART_RX_CAP);
    }
    s_rx_read = rd;
    s_rx_bytes += n;
    return n;
}

extern "C" bool link_uart_take_overflow(void)
{
    bool v = s_overflow;
    s_overflow = false;
    return v;
}

extern "C" uint32_t link_uart_rx_bytes(void)
{
    return s_rx_bytes;
}

extern "C" uint32_t link_uart_overflow_count(void)
{
    return s_overflow_count;
}

extern "C" uint32_t link_uart_tx_fail(void)
{
    return s_tx_fail;
}

/* ------------------------------------------------------------------ */
/* 发送 (只有 status 任务调用)                                        */
/* ------------------------------------------------------------------ */

static void link_uart_write_frame(const uint8_t *buf, uint16_t len)
{
    size_t n = (size_t)Serial1.write(buf, len);
    if (n != (size_t)len) {
        s_tx_fail++;
    }
}

extern "C" void link_uart_send_status(const status_frame_t *st, uint8_t seq)
{
    if (st == NULL) {
        return;
    }
    uint8_t buf[STATUS_FRAME_SIZE];
    proto_encode_status(buf, seq, st->rssi, st->state,
                        st->yaw_deg, st->pitch_deg, st->temp_mos, st->err_code);
    link_uart_write_frame(buf, STATUS_FRAME_SIZE);
}

extern "C" void link_uart_send_motor(float pos_rad, float vel_rad_s, float tau,
                                     uint16_t can_fail, uint8_t safety, uint8_t cmd_drops,
                                     uint8_t arm_echo, uint8_t seq)
{
    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_motor_ext2(buf, pos_rad, vel_rad_s, tau, can_fail,
                            safety, cmd_drops, arm_echo, seq);
    link_uart_write_frame(buf, CTRL_FRAME_SIZE);
}

extern "C" void link_uart_send_param_reply(uint8_t rid, bool ok, const uint8_t value[4], uint8_t seq)
{
    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_param_reply(buf, rid, ok ? 1u : 0u, value, seq);
    link_uart_write_frame(buf, CTRL_FRAME_SIZE);
}
