/**
 * @file    bridge_status.h
 * @brief   桥接端的只读状态快照 —— 给 OLED 显示用
 *
 *  桥接任务在跑 FreeRTOS 任务, 显示也在自己节奏刷新。
 *  用 getter 一次性拷贝出一份快照, 避免显示代码读到"半更新"的字段。
 */

#ifndef BRIDGE_STATUS_H
#define BRIDGE_STATUS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     link_up;          /* 主机是否在线 */
    int8_t   rssi;             /* 最近一次 ESP-NOW 的 RSSI */
    uint32_t rx_ok;            /* 收到的有效控制帧总数 */
    uint32_t rx_bad;           /* 校验失败/长度错的帧数 */
    uint32_t rx_dup;           /* 重复/乱序被丢弃的帧数 */
    uint32_t fwd_ok;           /* 成功转发给 STM32 的帧数 */
    uint32_t failsafe_sent;    /* 注入失能帧的次数 */
    uint32_t status_rx;        /* 收到 STM32 反馈帧的次数 */
    bool     have_stm32;       /* 是否收到过 STM32 反馈 */
    float    stm32_yaw_deg;    /* STM32 回传的电机角度 */
    uint8_t  stm32_temp;       /* STM32 回传的 MOS 温度 */
    uint8_t  stm32_err;        /* STM32 回传的错误码 */
    uint8_t  stm32_state;      /* STM32 回传的链路状态枚举 */
    uint32_t uptime_ms;        /* 桥接运行时长 */
} bridge_status_t;

/** 取一份当前状态快照 */
void bridge_get_status(bridge_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_STATUS_H */
