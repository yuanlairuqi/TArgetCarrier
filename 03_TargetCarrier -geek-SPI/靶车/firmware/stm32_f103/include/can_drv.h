/**
 * @file    can_drv.h
 * @brief   STM32F103 bxCAN 裸寄存器驱动 + 多任务发送协调
 *
 *  为什么不用 Arduino 的 CAN 库:
 *    1. 这里只需要最朴素的"发一帧/收一帧", 直接写寄存器没有中间层, 时序可控
 *    2. 避免依赖第三方库版本, 换编译器/换框架都能直接编
 *    3. 波特率配置要精确可核对 (见 config.h 的推导)
 *
 *  ★ 多任务下的邮箱协调 (本轮新增):
 *    以前主循环和 TIM2 中断都会直接调 can_drv_send(), 两者同时抢 TSR 的
 *    TME 位 + 写同一个邮箱, 极端情况下会把两个帧的数据混在一个邮箱里。
 *    现在:
 *      - 只有 ctrl 任务通过 lc_can_tx 策略调用 can_drv_send() (单一发送者);
 *      - can_drv_send() 本身再做一次"选邮箱 + 写寄存器"的短临界区保护;
 *      - CAN 恢复由 can 任务持锁完成, 恢复期间置 recovering 让控制任务停发;
 *      - 任何中断都不调用本文件的发送函数 (中断里只允许"请求", 由任务执行)。
 *
 *  硬件: CAN1, PB8=CRX / PB9=CTX (重映射), 1 Mbps, 正常模式
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

/** 一次完成轮询的结果 */
typedef struct {
    uint8_t completed;    /* 本次检测到完成的邮箱数 */
    uint8_t ok;           /* 其中总线发送成功 (收到 ACK) 的邮箱数 */
    uint8_t failed;       /* 其中失败/中止的邮箱数 */
} can_tx_done_t;

/**
 * @brief 初始化 CAN1 (GPIO + 时钟 + bxCAN + 过滤器) 与发送互斥量, 1Mbps
 * @return true 成功进入正常模式
 */
bool can_drv_init(void);

/**
 * @brief 发送一帧标准帧 (非阻塞, 写邮箱后立即返回)
 * @return true = 已写入某个空闲邮箱
 * @note 只能在任务上下文调用; 调用方应已持有 can_drv_lock()。
 */
bool can_drv_send(uint32_t id, const uint8_t *data, uint8_t len);

/**
 * @brief 获取/释放"CAN 发送与恢复"互斥量
 * @param timeout_ms 0 = 不阻塞 (控制任务用); 恢复任务可用较大值
 */
bool can_drv_lock(uint32_t timeout_ms);
void can_drv_unlock(void);

/** 中止三个邮箱里所有挂起的发送 (需持锁) */
void can_drv_abort_all(void);

/** 恢复状态标志 (恢复期间控制任务不得发送) */
void can_drv_set_recovering(bool on);
bool can_drv_is_recovering(void);

/**
 * @brief 轮询发送完成情况 (TXOK/ALST/TERR + TME)
 *        必须在任务上下文、持锁时调用。
 */
can_tx_done_t can_drv_poll_tx(void);

/** 尝试取出一帧收到的报文 (从 FIFO0) */
bool can_drv_receive(can_frame_t *out);

/** 最近一次"提交失败"计数 (三个邮箱都占着) */
uint32_t can_drv_tx_fail_count(void);

/** bxCAN 是否处于正常模式且没进总线关闭状态 */
bool can_drv_is_healthy(void);

/** 是否处于 bus-off (需要恢复) */
bool can_drv_bus_off(void);

/**
 * @brief 重新初始化 CAN 控制器 (需持锁; 会忙等 INAK, 只在任务里调用)
 */
bool can_drv_recover(void);

/** 总线完成 (收到 ACK) / 失败 (ALST/TERR) 累计计数 */
uint32_t can_drv_bus_ok_count(void);
uint32_t can_drv_bus_fail_count(void);

#ifdef __cplusplus
}
#endif

#endif /* CAN_DRV_H */
