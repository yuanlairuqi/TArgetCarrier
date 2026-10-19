/**
 * @file    can_drv.h
 * @brief   STM32F103 bxCAN 裸寄存器驱动
 *
 *  为什么不用 Arduino 的 CAN 库:
 *    1. 这里只需要最朴素的"发一帧/收一帧", 直接写寄存器没有中间层, 时序可控
 *    2. 避免依赖第三方库版本, 换编译器/换框架都能直接编
 *    3. 波特率配置要精确可核对 (见 config.h 的推导)
 *
 *  硬件: CAN1, PA11=TX(复用推挽), PA12=RX(上拉输入), 正常模式
 */

#ifndef CAN_DRV_H
#define CAN_DRV_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 一帧经典 CAN 报文 */
typedef struct {
    uint32_t id;          /* 11 位标准 ID */
    uint8_t  data[8];
    uint8_t  len;         /* 0..8 */
} can_frame_t;

/**
 * @brief 初始化 CAN1 (GPIO + 时钟 + bxCAN + 过滤器), 1Mbps
 * @return true 成功进入正常模式
 */
bool can_drv_init(void);

/**
 * @brief 发送一帧标准帧 (非阻塞, 写邮箱后立即返回)
 * @return true = 已写入某个空闲邮箱
 */
bool can_drv_send(uint32_t id, const uint8_t *data, uint8_t len);

/**
 * @brief 尝试取出一帧收到的报文 (从 FIFO0, 主循环里调用)
 * @return true = 取到一帧
 */
bool can_drv_receive(can_frame_t *out);

/**
 * @brief 最近一次发送失败计数 (邮箱全满或总线错误)
 */
uint32_t can_drv_tx_fail_count(void);

/**
 * @brief bxCAN 是否处于正常模式且没进总线关闭状态
 */
bool can_drv_is_healthy(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_DRV_H */
