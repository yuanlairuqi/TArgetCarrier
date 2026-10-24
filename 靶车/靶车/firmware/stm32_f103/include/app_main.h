/**
 * @file    app_main.h
 * @brief   STM32F103 应用层: 链路命令 -> MIT 控制 -> CAN -> 电机
 */

#ifndef APP_MAIN_H
#define APP_MAIN_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 上电初始化: CAN / USART1 / 1kHz 控制定时器 */
void app_setup(void);

/** 主循环体 (非阻塞, 由 Arduino loop() 调用) */
void app_loop(void);

/* ------------------------------------------------------------------ */
/* 只读状态快照 —— 给 OLED 显示用                                       */
/* ------------------------------------------------------------------ */
/* 这些变量在控制中断里被更新, 所以用 getter 拷贝出来,
 * 不要在显示代码里直接读内部变量, 避免撕裂读。 */

typedef struct {
    float   target_deg;      /* 目标偏航角 (度) */
    float   actual_deg;      /* 电机反馈角度 (度) */
    float   target_vel_dps;  /* 目标角速度前馈 (度/秒) */
    float   kp;
    float   kd;
    uint8_t temp_mos;        /* MOS 温度 (℃) */
    uint8_t err_code;        /* 电机错误码, 0 = 正常 */
    bool    enabled;         /* 是否已使能 */
    bool    fault;           /* 是否故障 */
    bool    estop;           /* 是否急停 */
    bool    feedback_valid;  /* 是否收到过电机反馈 */
    uint32_t ctrl_ticks;     /* 控制节拍计数 (1kHz) */
} app_status_t;

/** 取一份当前状态快照 */
void app_get_status(app_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* APP_MAIN_H */
