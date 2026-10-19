/**
 * @file    app_config.h
 * @brief   靶车 —— M5Stamp S3 桥接端 (接收机) 编译期配置
 *
 *  桥接职责:
 *    1. ESP-NOW 收主机控制帧 -> 校验 -> 从 UART 转发给 STM32
 *    2. 接收 STM32 的反馈帧 -> 打印 (可选回传主机)
 *    3. 链路看门狗: 主机失联立刻向 STM32 注入"失能帧", 不让云台继续动
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
#define LINK_UART_TX_TIMEOUT_MS   5

/* ---------------- 看门狗 ---------------- */
/* 主机失联超过该时间 -> 向 STM32 注入失能帧 */
#define BRIDGE_LINK_TIMEOUT_MS    300
/* 失联期间注入失能帧的间隔 (比超时短得多, 保证 STM32 侧收得到) */
#define BRIDGE_FAILSAFE_PERIOD_MS 50

/* ---------------- 调试输出 ---------------- */
#define BRIDGE_DEBUG              1        /* 0 = 关掉全部串口打印 */
#define BRIDGE_LOG_PERIOD_MS      500      /* 周期打印间隔 */
#define BRIDGE_RX_QUEUE_DEPTH     16       /* ESP-NOW 回调 -> 主任务 的排队深度 */

#endif /* APP_CONFIG_H */
