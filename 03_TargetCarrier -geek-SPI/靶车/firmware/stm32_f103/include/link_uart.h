/**
 * @file    link_uart.h
 * @brief   STM32 与 M5Stamp 桥接端的 USART1 链路 (FreeRTOS 适配层)
 *
 *  分工:
 *    - 接收: USART1 + DMA1_Channel5 循环搬运到 512B 环形缓冲;
 *      link 任务每 1ms 用 link_uart_read() 取字节喂给 lib/linkcore 的成帧器。
 *      DMA 在后台搬运, 所以 OLED/日志等慢任务不会丢字节; 但如果任务滞后
 *      超过环形缓冲, 会由 link_uart_read() 返回"溢出"信号, 上层必须丢弃半帧、
 *      重新同步 (旧实现没有这个检测, 会把旧字节当成新命令)。
 *    - 发送: 只允许 status 任务调用 link_uart_send_*() (单一写者, 无锁);
 *      检查 write 返回值, 短写计入 tx_fail。
 *
 *  保留的取舍 (与旧版一致):
 *    - 不用核心的 uart_attach_rx_callback(): 它会独占 RX 并让 Serial1.read()
 *      失效, 且拿不到 serial_t*; 这里直接操作 USART1/DMA 寄存器更可控。
 */

#ifndef LINK_UART_H
#define LINK_UART_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 USART1 (PA9=TX, PA10=RX, 921600 8N1) 并启动 DMA 循环接收 */
void link_uart_init(void);

/** DMA 环大小 (字节) */
#define LINK_UART_RX_CAP 512u

/**
 * @brief 从 DMA 环形缓冲取走最多 max 字节 (link 任务调用)
 * @return 实际取到的字节数
 */
uint16_t link_uart_read(uint8_t *out, uint16_t max);

/** 本周期是否发生过"消费滞后/被覆盖"; 返回 true 后自动清零 */
bool link_uart_take_overflow(void);

/** 调试计数 */
uint32_t link_uart_rx_bytes(void);
uint32_t link_uart_overflow_count(void);
uint32_t link_uart_tx_fail(void);

/** 发送一帧状态帧 (status 任务专用) */
void link_uart_send_status(const status_frame_t *st, uint8_t seq);

/** 发送电机实时量帧 (扩展安全位) */
void link_uart_send_motor(float pos_rad, float vel_rad_s, float tau,
                          uint16_t can_fail, uint8_t safety, uint8_t cmd_drops,
                          uint8_t arm_echo, uint8_t seq);

/** 发送参数回包 */
void link_uart_send_param_reply(uint8_t rid, bool ok, const uint8_t value[4], uint8_t seq);

#ifdef __cplusplus
}
#endif

#endif /* LINK_UART_H */
