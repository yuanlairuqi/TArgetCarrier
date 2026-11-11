/**
 * @file    app_tasks.h
 * @brief   STM32 FreeRTOS 任务划分与共享数据契约
 *
 *  任务表 (与文档 docs/通信可靠性与FreeRTOS移植说明.md 一致):
 *
 *   | 任务   | 优先级 | 触发/周期      | 栈(words) | 职责 |
 *   |--------|--------|----------------|-----------|------|
 *   | ctrl   | 5 最高 | TIM2 1kHz 通知 | 384       | 看门狗/ARM/CAN 发送协调/斜坡/MIT 打包 |
 *   | link   | 4      | 1ms 轮询       | 320       | USART1 DMA 消费、成帧、序号/过期、入队 |
 *   | can    | 4      | 1ms 轮询       | 256       | CAN 接收、电机反馈、参数回包、总线恢复 |
 *   | status | 3      | 20ms           | 320       | 50Hz 状态帧、20Hz 电机帧、参数回包 (唯一 UART 发送者) |
 *   | ui     | 1 最低 | 100ms/500ms    | 384       | OLED、调试日志、栈余量诊断 |
 *
 *  共享数据所有权:
 *   - 控制状态 (目标/斜坡/授权/CAN 发送表) 只由 ctrl 任务写;
 *   - 电机反馈只由 can 任务写;
 *   - 两者通过 app_rt_state_t + 短临界区 (taskENTER_CRITICAL) 交换"一致快照",
 *     不依赖 volatile 逐个字段读 (float 会撕裂, 组合字段会自相矛盾)。
 *   - status/ui 任务只读快照副本, 不在锁内做 OLED/串口等慢操作。
 */

#ifndef APP_TASKS_H
#define APP_TASKS_H

#include <stdint.h>
#include <stdbool.h>

#include "config.h"   /* CTRL_TIMEOUT_MS 等硬件/链路常量, 用于下面的编译期一致性检查 */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------- 优先级 ---------------- */
#define APP_PRIO_CTRL     5
#define APP_PRIO_LINK     4
#define APP_PRIO_CAN      4
#define APP_PRIO_STATUS   3
#define APP_PRIO_UI       1

/* ---------------- 栈 (words, StackType_t=4B) ---------------- */
#define APP_STACK_CTRL_WORDS    384   /* 1536 B: 浮点 MIT 打包 + CAN 协调 */
#define APP_STACK_LINK_WORDS    320   /* 1280 B: 成帧 + 解码 + 队列 */
#define APP_STACK_CAN_WORDS     256   /* 1024 B: 反馈解析 + 恢复 */
#define APP_STACK_STATUS_WORDS  320   /* 1280 B: snprintf + UART 发送 */
#define APP_STACK_UI_WORDS      384   /* 1536 B: OLED + dbg_printf */

/* ---------------- 通知位 ---------------- */
#define APP_NOTIFY_CTRL_TICK    (1u << 0)

/* ---------------- 周期 ---------------- */
#define APP_STATUS_PERIOD_MS    20u    /* 状态帧 50 Hz */
#define APP_MOTOR_PERIOD_MS     50u    /* 电机量 20 Hz */
#define APP_UI_PERIOD_MS        100u   /* OLED 10 Hz */
#define APP_LOG_PERIOD_MS       500u   /* 调试日志 2 Hz */
#define APP_STACK_DIAG_PERIOD_MS 5000u /* 栈余量诊断 */

/* ---------------- 保护时限 (与主/桥接/文档统一) ---------------- */
#define APP_CTRL_TIMEOUT_MS     300u   /* 无有效控制帧 -> 撤销授权并失能 */
#define APP_CMD_MAX_AGE_MS      100u   /* 出队命令的允许年龄 */
#define APP_FB_TIMEOUT_MS       200u   /* 使能时电机反馈超时 */
#define APP_FB_GRACE_MS         1000u  /* 上电/清错后首次反馈宽限 */
#define APP_CAN_RECOVER_THRESHOLD 64u  /* 连续发送失败 -> 请求恢复 */
#define APP_CAN_RECOVER_PERIOD_MS 2000u
#define APP_DISABLE_REPEAT       3u    /* 失能后补发帧数 (间隔约 2ms) */

/* 编译期把"代码/配置/文档必须一致"的时限钉死: 任何一端改了数值而没同步,
 * 这里会直接编译失败, 而不是等现场出现"文档写 300ms、实际 1s"的问题。 */
#if APP_CTRL_TIMEOUT_MS != 300u
#error "APP_CTRL_TIMEOUT_MS 必须与主机/桥接/文档统一为 300ms"
#endif
#if APP_CMD_MAX_AGE_MS != 100u
#error "APP_CMD_MAX_AGE_MS 必须与主机/桥接/文档统一为 100ms"
#endif
#if APP_FB_TIMEOUT_MS != 200u
#error "APP_FB_TIMEOUT_MS 必须与文档统一为 200ms"
#endif
#if APP_CTRL_TIMEOUT_MS != CTRL_TIMEOUT_MS
#error "APP_CTRL_TIMEOUT_MS 与 config.h 的 CTRL_TIMEOUT_MS 不一致"
#endif

/* ---------------- 安全原因 (对外诊断, 与主机 JSON safe_reason 对应) ---------------- */
typedef enum {
    APP_SAFE_NONE = 0,
    APP_SAFE_LINK_LOST = 1,     /* 控制链路 300ms 无有效帧 */
    APP_SAFE_STM_FB = 2,        /* 电机反馈超时 */
    APP_SAFE_CAN = 3,           /* CAN 故障/恢复 */
    APP_SAFE_ESTOP = 4,         /* 急停 */
    APP_SAFE_FAULT = 5,         /* 电机自报故障 */
    APP_SAFE_RTOS = 6           /* 栈溢出/断言等 RTOS 级异常 */
} app_safe_reason_t;

/* ---------------- 一致快照 ----------------
 * 由 ctrl 任务在锁内整体更新, status/ui 在锁内整体拷贝。
 * 字段都是"人类可读单位" (度), 避免显示层再做单位换算。 */
typedef struct {
    float    target_deg;
    float    actual_deg;
    float    target_vel_dps;
    float    kp;
    float    kd;
    uint8_t  temp_mos;
    uint8_t  err_code;
    uint8_t  arm_token;
    uint8_t  safe_reason;
    uint16_t flags;
    uint32_t ctrl_ticks;
    uint32_t cmd_drops;       /* 过期/丢弃命令累计 (诊断) */
    uint32_t gains_rx;        /* 累计收到的增益帧 */
    uint32_t fb_age_ms;
    uint16_t can_fail;
    bool     armed;
    bool     enabled;
    bool     estop;
    bool     fault;
    bool     feedback_valid;
    bool     feedback_fresh;
    bool     link_ok;
    bool     can_recovering;
    bool     arm_reject;      /* 曾拒绝过重放/旧令牌 */
} app_rt_state_t;

#ifdef __cplusplus
}
#endif

#endif /* APP_TASKS_H */
