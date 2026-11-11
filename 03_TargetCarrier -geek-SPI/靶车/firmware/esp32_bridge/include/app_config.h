/**
 * @file    app_config.h
 * @brief   靶车 —— M5Stamp S3 桥接端 (接收机) 编译期配置
 *
 *  桥接职责:
 *    1. ESP-NOW 收主机控制帧 -> 校验 -> 从 UART **原样**转发给 STM32 (不重编码/不改 seq)
 *    2. 接收 STM32 的反馈帧 -> 新鲜 (<=300ms) 时回传主机 / 打印; 过期后不再重发缓存
 *    3. 安全保护: 主机静默 >BRIDGE_MASTER_TIMEOUT_MS -> 进入 FAILSAFE, 每
 *       BRIDGE_FAILSAFE_PERIOD_MS 注入一帧 flags=CTRL_FLAG_FAILSAFE 的失能帧,
 *       直到收到主机明确的 ENABLE=0 帧且链路恢复
 *
 *  ★ M5Stamp-S3Bat DIP 的接线坑 (官方资料 07 号文档「管脚映射」):
 *      - G43 = U0TX, G44 = U0RX, 但 G44 同时被 M5PM1 电源管理芯片当 SCL 用,
 *        所以 UART0(G43/G44) 不要拿来接 STM32, 否则和电源管理抢线。
 *      - G13/G15 是板载 I2C(M5PM1), 也不能用。
 *    因此本固件用排针上的普通 GPIO:
 *        TXD = G1  (排针第 2 脚)  -> STM32 PA10 (USART1_RX)
 *        RXD = G2  (排针第 4 脚)  -> STM32 PA9  (USART1_TX)
 *    如果你用的是普通 M5StampS3 (非 Bat DIP), 也可以改用 TXD2=1/RXD2=2, 一样。
 *
 *   调试串口用 USB CDC (Serial), 与 STM32 链路完全独立。
 */

#ifndef APP_CONFIG_H
#define APP_CONFIG_H

/* ---------------- ESP-NOW ---------------- */
/* 必须与 esp32_master/app_config.h 里的 ESPNOW_CHANNEL 一致 */
#define ESPNOW_CHANNEL            6

/* 主机 MAC 白名单: 留空(全 0) 表示不过滤, 接受任何发来的控制帧。
 * 强烈建议跑通后把主机的真实 MAC 填进来, 避免现场其它 ESP-NOW 设备干扰。 */
#define ESPNOW_FILTER_MASTER      0
#define ESPNOW_MASTER_MAC_0       0x00
#define ESPNOW_MASTER_MAC_1       0x00
#define ESPNOW_MASTER_MAC_2       0x00
#define ESPNOW_MASTER_MAC_3       0x00
#define ESPNOW_MASTER_MAC_4       0x00
#define ESPNOW_MASTER_MAC_5       0x00

/* ---------------- 与 STM32 的串口 ---------------- */
#define LINK_UART_INDEX           1        /* Serial1 */
#define LINK_BAUD                 921600   /* 与 stm32_f103 的 config.h 保持一致 */
#define LINK_PIN_TXD              1        /* GPIO1 -> STM32 PA10 */
#define LINK_PIN_RXD              2        /* GPIO2 <- STM32 PA9  */
/* 注意: 原先这里还有一个"串口发送超时"常量, 已经删除 —— 它是死常量,
 *       HardwareSerial 并没有"发送超时"配置入口, 留着只会让人误以为配过。 */

/* ==================================================================
 *  三端保护时限 —— 数值必须与
 *  verification/freertos_port_20261024/DESIGN_CONTRACT.md §2 表格逐项一致。
 *  改这里必须同步改主机/STM32/文档, 否则会出现"一端以为断了、一端还在跑"。
 * ================================================================== */

/* 命令有效期: 入队时刻算起超过该毫秒数, 出队时直接丢弃 (不再当新命令执行) */
#define CTRL_CMD_MAX_AGE_MS        100u
/* 主机静默超过该时间 -> 桥接进入 FAILSAFE (锁存, 见 main.cpp) */
#define BRIDGE_MASTER_TIMEOUT_MS   300u
/* FAILSAFE 期间 / 初始化降级期间 注入失能帧的间隔 (20Hz) */
#define BRIDGE_FAILSAFE_PERIOD_MS   50u
/* STM32 反馈年龄超过该值 -> STM_FRESH=0, 不再转发缓存 STATUS */
#define BRIDGE_STM_FRESH_MS        300u
/* 桥接 -> 主机 的新鲜度帧 (PROTO_MSG_BRIDGE_STATUS) 发送周期 (10Hz) */
#define BRIDGE_UPLINK_PERIOD_MS    100u
/* ESP-NOW 上行发送在途超时: 回调迟迟不来时 250ms 后放行并计超时 (不能无限等) */
#define ESPNOW_CB_TIMEOUT_MS       250u

/* ---------------- 接收队列 (契约 §5.2) ---------------- */
/* 安全帧: CTRL 里带 ESTOP/FAILSAFE/CLEAR_ERR/SET_ZERO 或 ENABLE 边沿的帧。
 * 深度只有 4, 满了就"顶掉最旧的一条", 保证最新的安全命令一定进队。 */
#define BRIDGE_SAFETY_QUEUE_DEPTH   4
/* 普通运动/增益/参数帧; 满了丢新帧并计数 */
#define BRIDGE_CMD_QUEUE_DEPTH     16
/* 桥接任务每轮最多处理的普通帧数: 防止队列持续满载时周期安全动作被饿死 */
#define BRIDGE_CMD_BATCH            8u
/* 桥接任务轮询周期: 2ms 对 50Hz 控制帧/100ms 上行/50ms 注入都足够精确 */
#define BRIDGE_TASK_TICK_MS         2u

/* ---------------- STM32 串口接收保护 ---------------- */
/* 接收积压超过该字节数 -> 判定为消费滞后, 丢掉半帧并让成帧器重新同步 */
#define BRIDGE_UART_BACKLOG_MAX    64
/* 半帧超时: 已收到帧头但后续字节超过该时间没到 -> 丢半帧, 等下一次帧头 */
#define BRIDGE_UART_FRAME_GAP_MS   20u

/* ---------------- ESP-NOW 上行 (桥接 -> 主机) ---------------- */
/* 上行 peer (广播) 注册失败后的重试间隔 */
#define BRIDGE_PEER_RETRY_MS       1000u
/* STM32 STATUS 上行最小间隔。实现选择 (不在契约 §2 表内):
 * STM32 是 50Hz 状态帧, 全转发会把"一条在途"的上行时隙占满,
 * 桥接新鲜度帧就抢不到时隙, 主机反而会误判桥接失联。限制到 10Hz。 */
#define BRIDGE_STM_UP_PERIOD_MS    100u

/* ---------------- 调试输出 ---------------- */
#define BRIDGE_DEBUG              1        /* 0 = 关掉全部串口打印 */
#define BRIDGE_LOG_PERIOD_MS      500      /* 周期打印间隔 */

/* ---------------- 家里 WiFi (可选) ---------------- */
/* 和主机连同一个 WiFi 后, ESP-NOW 信道自动跟随路由器 (两端一致)。
 * 配网: 串口发  J <WiFi名>,<密码>  (存 NVS 后自动重启); D 清除。 */
#define WIFI_CONNECT_TIMEOUT_MS   12000u

#endif /* APP_CONFIG_H */
