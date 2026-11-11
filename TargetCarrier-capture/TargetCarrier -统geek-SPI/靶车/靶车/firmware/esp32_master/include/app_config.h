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
/* 桥接 (M5Stamp S3) 的真实 MAC: 00:00:00:00:00:00
 * 用单播才有 ESP-NOW 的 ACK 回调 -> tx_cb_ok/tx_cb_fail 才是真实链路结果。
 * 若换了板子导致发不出去, 固件会在"桥接新鲜度帧过期且控制帧回调全部失败"时
 * 自动退回广播 (判据见 main.cpp 的 2a2, 不再用固定帧数魔法数)。 */
#define ESPNOW_PEER_MAC_0         0xAC
#define ESPNOW_PEER_MAC_1         0x27
#define ESPNOW_PEER_MAC_2         0x6E
#define ESPNOW_PEER_MAC_3         0xD3
#define ESPNOW_PEER_MAC_4         0x32
#define ESPNOW_PEER_MAC_5         0x80
#define ESPNOW_USE_BROADCAST      0

/* 主机自身要固定成 STA 并且不给 WiFi 配置任何 AP，避免信道漂移 */
#define ESPNOW_WIFI_MODE_STA      1

/* ---------------- 网页控制 (内置 WiFi 热点) ---------------- */
/* 手机/电脑连这个热点, 浏览器打开 http://192.168.4.1 即可控制。
 * ★ 热点信道必须等于 ESPNOW_CHANNEL: ESP-NOW 和热点共用同一个射频,
 *   信道不一致会直接断掉 ESP-NOW 链路。 */
#define WEB_UI_ENABLE             1
#define WEB_AP_SSID               "TargetCar"
#define WEB_AP_PASS               "target123"      /* 至少 8 位 */
#define WEB_AP_CHANNEL            ESPNOW_CHANNEL

/* 连家里路由器的超时: 连上之后 ESP-NOW 会自动改用路由器的信道;
 * 连不上就退回热点配网 (TargetCar)。账号用串口命令 J <ssid>,<密码> 配一次。
 * ★ 现在不再"先阻塞等 WiFi 再开热点": 热点开机立即启动, 连路由器在后台慢慢试,
 *   所以手机打开页面不会再有十几秒的等待。 */
#define WIFI_CONNECT_TIMEOUT_MS   12000u
#define WIFI_STA_RETRY_MS         8000u     /* 连不上时多久重试一次 */

/* ---------------- 调参安全 (防止参数把电机逼到极限) ---------------- */
/* ★ 这些是"主机侧"的上限, 目的是防止网页/串口手滑把 kp 拉到 500 让云台自激:
 *     - 命令/滑块超过上限会被夹住并打印提示
 *     - 使能前要求 kd >= GIMBAL_KD_MIN_ENABLE (kd=0 位置控制一定震荡)
 *     - 参数变化在 50 Hz 控制循环里做斜坡, 不会一步跳变
 *   真要更大的 kp, 改这里的宏并重新编译, 这是刻意留的一道闸。 */
#define GIMBAL_KP_SAFE_MAX        200.0f
#define GIMBAL_KD_SAFE_MAX        3.0f
#define GIMBAL_KD_MIN_ENABLE      0.1f
#define PARAM_SLEW_DIV            8u        /* 每 20ms 逼近剩余差的 1/8, 约 0.3s 到稳定 */

/* ---------------- 控制律 ---------------- */
#define CTRL_LOOP_HZ              50                       /* ESP-NOW 发送频率 */
#define CTRL_LOOP_PERIOD_MS       (1000 / CTRL_LOOP_HZ)    /* 20ms */

/* ---------------- 主机侧保护时限 (契约 §2, 三端统一口径) ---------------- */
/* ★ 主机只做"进 SAFE / 撤销授权"的本地判定, 真正断使能在桥接和 STM32 里做;
 *   这里的数值必须与 bridge / STM32 的 300ms 一致, 否则会出现
 *   "一端已经失能、另一端还在按旧口径发使能"的危险窗口。
 *   以前这里有个 300ms 的"链路看门狗"宏, 但没有任何代码使用 (误导性死常量),
 *   现按用途拆成下面几个真正被 main.cpp 使用的宏。 */
#define MASTER_BRIDGE_STALE_MS    300u   /* 桥接新鲜度帧(0x07)超过 300ms 未更新 -> SAFE */
#define MASTER_STM_STALE_MS       300u   /* STM32 状态帧超过 300ms 未更新 -> SAFE */
#define ESPNOW_CB_TIMEOUT_MS      250u   /* ESP-NOW 发送回调在途超时 */
#define ESPNOW_CB_TIMEOUT_N       3u     /* 连续在途超时次数 >= 3 -> 计入 SAFE 原因 */

/* 控制命令最大年龄: 主机 WS 命令队列出队时也按这个值丢弃 (契约 §2 CTRL_CMD_MAX_AGE_MS);
 * 与桥接入队 / STM32 出队的 100ms 口径保持一致, 旧命令绝不当成新命令执行。 */
#define CTRL_CMD_MAX_AGE_MS       100u

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

/* ★ 默认轨迹 = 定点保持: 上电后云台停在 0°不动, 不会自己摆。
 *   要做靶就手动切: 串口发 1(正弦) / 2(三角扫描) / 3(圆周) / 4(八字)。
 *   之前默认是 SINE, 上电就摆 30°, 第一次接负载时太危险。 */
#define PROFILE_DEFAULT           PROFILE_STATIC
#define SINE_AMPLITUDE_DEG        10.0f    /* 摆动幅度 (保守默认, 需要大就 a 60) */
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
