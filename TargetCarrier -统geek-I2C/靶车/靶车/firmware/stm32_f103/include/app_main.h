/**
 * @file    app_main.h
 * @brief   STM32F103 应用层: 链路命令 -> MIT 控制 -> CAN -> 电机 (FreeRTOS 版)
 *
 *  对外只保留"启动"和"只读状态快照"两个入口; 任务划分见 app_tasks.h。
 *  业务逻辑 (控制/安全/状态机) 全部在这个文件里, 与 FreeRTOS/HAL 的耦合
 *  只通过 app_tasks.h 的任务表和 link_uart/can_drv 两个驱动接口。
 */

#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 参数读写请求 (link 任务解析 -> ctrl 任务转发 CAN 0x7FF) */
typedef struct {
    uint8_t op;        /* PROTO_PARAM_OP_* */
    uint8_t rid;
    uint8_t value[4];
} app_param_t;

/** 参数回包 (can 任务/Ctrl 任务 -> status 任务发送) */
typedef struct {
    uint8_t rid;
    uint8_t ok;
    uint8_t value[4];
} app_param_reply_t;

/**
 * @brief 初始化外设、创建任务/队列/互斥量, 然后启动 FreeRTOS 调度器。
 *        本函数正常情况下 **不返回** (调度器接管)。
 */
void app_setup(void);

/** 已初始化的诊断计数 (给显示用) */
typedef struct {
    float   target_deg;      /* 目标偏航角 (度) */
    float   actual_deg;      /* 电机反馈角度 (度) */
    float   target_vel_dps;  /* 目标角速度前馈 (度/秒) */
    float   kp;
    float   kd;
    uint8_t temp_mos;        /* MOS 温度 (℃) */
    uint8_t err_code;        /* 电机状态/故障码: 0=失能 1=使能, >=0x8 才是故障 */
    uint32_t gains_rx;       /* 累计收到多少帧增益 (kp/kd) */
    bool    enabled;         /* 是否已使能 */
    bool    fault;           /* 是否故障 */
    bool    estop;           /* 是否急停 */
    bool    feedback_valid;  /* 是否收到过电机反馈 */
    uint32_t ctrl_ticks;     /* 控制节拍计数 (1kHz) */
} app_status_t;

/** 取一份当前状态快照 (锁内整体拷贝, 不会撕裂) */
void app_get_status(app_status_t *out);

/**
 * @brief 栈溢出/断言等致命路径专用: 不取锁、不等待, 尽力发 3 帧失能命令。
 *        随后调用方会复位 MCU (复位后默认未授权/失能)。
 */
void app_emergency_disable_motor(void);

#ifdef __cplusplus
}
#endif

#endif /* APP_MAIN_H */
