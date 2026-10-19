/**
 * @file    link_uart.h
 * @brief   STM32 与 M5Stamp 桥接端的 USART1 链路
 *
 *  接收: USART1 中断驱动的字节状态机 (帧头 -> 满载 -> CRC/帧尾校验),
 *        校验通过才把帧交给主循环, 避免主循环里跑解析被时序打断。
 *  发送: 反馈帧直接写 DR, 发送前轮询 TXE 保证不覆盖。
 *
 *  为什么收在中断里做:
 *    921600bps = 每字节 10.85us。主循环的 1kHz 控制节拍是 1000us,
 *    如果靠主循环轮询, 一帧 17 字节要 184us, 中间的 CAN 控制节拍必然抖动。
 *    放中断里每个字节只花几微秒, 控制环不受影响。
 */

#ifndef LINK_UART_H
#define LINK_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    ctrl_payload_t payload;     /* 解析出的控制载荷 */
    uint8_t        seq;
    uint32_t       rx_ms;       /* 收到该帧的时间戳 */
} link_cmd_t;

/** 初始化 USART1 (PA9=TX, PA10=RX, 921600 8N1) + 打开 RXNE 中断 */
void link_uart_init(void);

/** 主循环每 1ms 调一次, 给接收打时间戳 */
void link_uart_tick_update(uint32_t now_ms);

/** 主循环每轮调一次: 从串口缓冲取字节喂状态机 */
void link_uart_poll(void);

/**
 * @brief 取出最新一帧有效控制帧 (主循环调用)
 * @return true = 期间收到过新帧
 */
bool link_uart_take_command(link_cmd_t *out);

/** 发送一帧反馈帧 */
void link_uart_send_status(const status_frame_t *st);

/* 统计量, 方便现场判断链路质量 */
uint32_t link_uart_rx_ok(void);
uint32_t link_uart_rx_bad(void);

#ifdef __cplusplus
}
#endif

#endif /* LINK_UART_H */
