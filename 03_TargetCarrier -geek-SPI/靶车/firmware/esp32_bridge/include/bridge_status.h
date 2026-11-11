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
    uint8_t  stm32_err;        /* STM32 回传的电机状态/故障码 (0=失能 1=使能, >=0x8 故障) */
    uint8_t  stm32_state;      /* STM32 回传的链路状态枚举 */
    uint32_t uptime_ms;        /* 桥接运行时长 */

    /* ---- v1.1: STM32 反馈新鲜度 / 安全锁 / 诊断 (供 OLED 显示) ----
     * 为什么需要这几个字段:
     *   桥接现在只在 STM32 反馈新鲜 (<=300ms) 时把 STATUS 转给主机,
     *   过期后不再重发最后一帧缓存。屏幕上必须能看出"STM32 到底还在不在回话",
     *   否则现场会误以为主机显示的旧数据是实时的。 */
    uint32_t stm32_age_ms;     /* STM32 最近一次反馈的年龄 (ms); 从未收到过为 0xFFFFFFFF */
    bool     stm32_fresh;      /* 年龄是否 <= BRIDGE_STM_FRESH_MS */
    bool     motion_blocked;   /* 运动被桥接锁住 (FAILSAFE 锁存 或 初始化降级), 期间不转发 ENABLE */
    bool     failsafe;         /* 正在以 20Hz 注入失能帧 (主机静默超时) */
    bool     degraded;         /* 初始化失败后的降级安全态 (队列/任务/上行 peer) */
    uint32_t overflows;        /* 串口接收溢出/半帧重同步次数 (诊断接线与负载) */
} bridge_status_t;

/** 取一份当前状态快照 */
void bridge_get_status(bridge_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BRIDGE_STATUS_H */
