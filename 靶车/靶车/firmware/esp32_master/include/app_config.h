/**
 * @file    app_config.h
 * @brief   靶车 —— ESP32 主机 (发射端) 编译期配置
 *
 *  主机职责:
 *    1. 自主产生云台运动指令 (正弦/三角扫描/圆/8字/定点), 不需要电脑参与
 *    2. 以 50Hz 通过 ESP-NOW 广播给 M5Stamp S3 从机
 *    3. 串口 (USB) 支持在线切换模式/调参, 方便现场调试
 *
 *  改这里就能改行为, 不用动业务代码。
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---------------- ESP-NOW 链路 ---------------- */
/* 三颗 ESP32 必须同信道。改这里时要同时改 bridge 的 app_config.h */
#define ESPNOW_CHANNEL            6

/* 从机 (M5Stamp S3) 的 MAC。留空则用广播地址，先用广播跑通链路，
 * 然后看串口打印的 "RX from xx:xx:.." 把真实 MAC 填进来。 */
#define ESPNOW_PEER_MAC_0         0xFF
#define ESPNOW_PEER_MAC_1         0xFF
#define ESPNOW_PEER_MAC_2         0xFF
#define ESPNOW_PEER_MAC_3         0xFF
#define ESPNOW_PEER_MAC_4         0xFF
#define ESPNOW_PEER_MAC_5         0xFF
#define ESPNOW_USE_BROADCAST      1

/* 主机自身要固定成 STA 并且不给 WiFi 配置任何 AP，避免信道漂移 */
#define ESPNOW_WIFI_MODE_STA      1

/* ---------------- 控制律 ---------------- */
#define CTRL_LOOP_HZ              50                       /* ESP-NOW 发送频率 */
#define CTRL_LOOP_PERIOD_MS       (1000 / CTRL_LOOP_HZ)    /* 20ms */

/* 链路看门狗: 主机端只做报警, 真正断使能在桥接和 STM32 里做 */
#define MASTER_LINK_TIMEOUT_MS    300

/* ---------------- 云台机械限位 ---------------- */
#define GIMBAL_YAW_MIN_DEG        (-180.0f)
#define GIMBAL_YAW_MAX_DEG        ( 180.0f)
#define GIMBAL_PITCH_MIN_DEG      ( -30.0f)
#define GIMBAL_PITCH_MAX_DEG      (  60.0f)

/* MIT 阻抗增益: 云台属于大惯量负载, kp 太小会跟不上, 太大会啸叫 */
#define GIMBAL_KP_DEFAULT         60.0f   /* 范围 0~500 */
#define GIMBAL_KD_DEFAULT         1.5f    /* 范围 0~5   */

/* ---------------- 运动轨迹默认参数 ---------------- */
typedef enum {
    PROFILE_STATIC = 0,   /* 定点保持 */
    PROFILE_SINE,         /* 正弦摆动 (最平滑, 适合做靶) */
    PROFILE_SWEEP,        /* 三角波扫描 (端点停留, 扫场用) */
    PROFILE_CIRCLE,       /* 圆周 (偏航+俯仰联动) */
    PROFILE_FIGURE8,      /* 8 字 */
    PROFILE_MANUAL,       /* 手动: 串口直接下角度 */
    PROFILE_COUNT
} profile_id_t;

#define PROFILE_DEFAULT           PROFILE_SINE
#define SINE_AMPLITUDE_DEG        30.0f    /* 摆动幅度 */
#define SINE_PERIOD_S             6.0f     /* 摆动周期 */
#define SINE_CENTER_DEG           0.0f

#define SWEEP_AMPLITUDE_DEG       60.0f
#define SWEEP_SPEED_DPS           20.0f    /* 扫描角速度 */
#define SWEEP_DWELL_MS            500      /* 端点停留 */

#define CIRCLE_RADIUS_DEG         20.0f
#define CIRCLE_YAW_CENTER_DEG     0.0f
#define CIRCLE_PITCH_CENTER_DEG   0.0f
#define CIRCLE_PERIOD_S           8.0f

#define FIG8_YAW_AMP_DEG          40.0f
#define FIG8_PITCH_AMP_DEG        15.0f
#define FIG8_PERIOD_S             10.0f

/* ---------------- 串口 ---------------- */
#define SERIAL_BAUD               460800   /* 只用于打印/调参, 不影响 ESP-NOW */
#define SERIAL_TASK_INTERVAL_MS   2

/* 开这个宏, 每 500ms 打印一行遥测, 现场排查用; 正式跑可关掉减少串口占用 */
#define MASTER_ENABLE_TELEMETRY_LOG   1
#define TELEMETRY_LOG_PERIOD_MS       500

#endif /* APP_CONFIG_H */
