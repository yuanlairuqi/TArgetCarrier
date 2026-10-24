/**
 * @file    main.cpp
 * @brief   靶车 —— M5Stamp S3 桥接端 (ESP-NOW -> UART -> STM32)
 *
 *   数据流:
 *     主机 ESP-NOW  ──►  on_espnow_recv() 回调 (只做校验+入队, 绝不阻塞)
 *                              │  xQueueSendFromISR
 *                              ▼
 *                        bridge_task (独立任务)
 *                              │  解包/查重/统计
 *                              ▼
 *                        Serial1 ──► STM32 USART1 ──► CAN ──► DM-G6220
 *
 *   反向:
 *     Serial1 ◄── STM32 反馈帧 ──► 解析 ──► 打印 / (可选) ESP-NOW 回传主机
 *
 *   安全设计:
 *     - 主机 300ms 无有效帧 -> 桥接每 50ms 向 STM32 注入失能帧 (kp/kd=0, 不使能)
 *     - STM32 侧另有独立超时兜底, 两层保护, 单点失效不会让云台失控
 *     - 序号查重: 丢弃重复/倒序帧, 防止 ESP-NOW 重传把旧目标顶回来
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "protocol.h"
#include "app_config.h"
#include "bridge_status.h"
#include "display.h"

/* 允许用 ESP_ARDUINO_VERSION_MAJOR 做条件编译; 老核心不带这个宏 */
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

/* ================================================================== */
/* 状态                                                               */
/* ================================================================== */

/* ESP-NOW 回调 -> bridge_task 的队列元素 */
typedef struct {
    uint8_t data[CTRL_FRAME_SIZE];
    int8_t  rssi;
} rx_item_t;

static QueueHandle_t     g_rx_queue;
static TaskHandle_t      g_bridge_task;

static volatile uint32_t g_rx_ok          = 0;   /* 有效控制帧 */
static volatile uint32_t g_rx_bad         = 0;   /* 校验失败/长度错 */
static volatile uint32_t g_rx_dup         = 0;   /* 重复或乱序 */
static volatile uint32_t g_fwd_ok         = 0;   /* 成功转发给 STM32 */
static volatile uint32_t g_failsafe_sent  = 0;   /* 注入失能帧次数 */
static volatile uint32_t g_status_rx      = 0;   /* 收到 STM32 反馈帧 */
static volatile int8_t   g_last_rssi      = 0;

static volatile uint32_t g_last_valid_ms   = 0;   /* 上次收到有效控制帧的时刻 */
static volatile bool     g_link_up         = false;
static volatile bool     g_failsafe_active = false;

static uint8_t           g_last_seq        = 0;
static bool              g_have_seq        = false;
static uint8_t           g_fwd_seq         = 0;

static uint32_t          g_last_log_ms     = 0;

/* 保护 g_last_status / g_have_status (桥接任务写, 显示任务读) */
static portMUX_TYPE      g_status_mux      = portMUX_INITIALIZER_UNLOCKED;

/* 反馈帧解析状态机 */
static uint8_t           g_fb_buf[RX_FRAME_SIZE];
static uint8_t           g_fb_len          = 0;
static uint8_t           g_fb_state        = 0;
static status_frame_t    g_last_status;
static bool              g_have_status     = false;

/* 前置声明: setup() 里创建任务时要用到 */
static void display_task(void *arg);

/* 主机 MAC 白名单。只在 ESPNOW_FILTER_MASTER=1 时才会被用到,
 * 默认为 0(不过滤), 所以这里加 unused 属性避免 -Wunused 警告。 */
#if ESPNOW_FILTER_MASTER
static uint8_t g_master_mac[6] = {
    ESPNOW_MASTER_MAC_0, ESPNOW_MASTER_MAC_1, ESPNOW_MASTER_MAC_2,
    ESPNOW_MASTER_MAC_3, ESPNOW_MASTER_MAC_4, ESPNOW_MASTER_MAC_5
};
#endif

/* ================================================================== */
/* ESP-NOW 接收回调 —— 禁止耗时操作 (不能在这里写串口!)               */
/* ================================================================== */

#if ESP_ARDUINO_VERSION_MAJOR >= 3
/* Arduino-ESP32 3.x 使用 esp_now_recv_info_t, 里面带 RSSI */
static void bridge_on_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    const uint8_t *mac = info->src_addr;
    int8_t rssi = (int8_t)info->rx_ctrl->rssi;
#else
/* Arduino-ESP32 2.x: 回调里拿不到 RSSI, 只能从 WiFi 侧读 */
static void bridge_on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    int8_t rssi = 0;
#endif
    /* 长度不对直接丢, 不做任何解析 */
    if (len != (int)CTRL_FRAME_SIZE) {
        g_rx_bad++;
        return;
    }

#if ESPNOW_FILTER_MASTER
    if (memcmp(mac, g_master_mac, 6) != 0) {
        g_rx_bad++;
        return;
    }
#endif

    /* 先把校验做掉, 坏帧根本不进队列, 省得主任务白跑 */
    ctrl_payload_t pl;
    uint8_t seq;
    if (!proto_decode_ctrl(data, (uint16_t)len, &pl, &seq)) {
        g_rx_bad++;
        return;
    }

    g_last_rssi = rssi;

    rx_item_t item;
    memcpy(item.data, data, CTRL_FRAME_SIZE);
    item.rssi = rssi;

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(g_rx_queue, &item, &woken) != pdTRUE) {
        /* 队列满: 主任务被卡住了, 丢帧保实时性, 由看门狗兜底 */
        g_rx_bad++;
    }
    if (woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

/* ================================================================== */
/* 串口: 向 STM32 发控制帧                                            */
/* ================================================================== */

static void uart_send_ctrl(const ctrl_payload_t *pl, uint16_t flags,
                           uint8_t mode, uint8_t seq)
{
    /* 把已解码的载荷重新封装成完整帧转发 (字节级一致, 不重新量化) */
    ctrl_target_t t;
    proto_payload_to_target(pl, &t);

    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_ctrl(buf, &t, flags, mode, seq);

    Serial1.write(buf, CTRL_FRAME_SIZE);
    Serial1.flush();     /* 17 字节 @921600 ≈ 185us, 不会明显阻塞 */
}

/* 失联保护: 注入一帧"不使能 + 增益全零", STM32 收到会立刻松掉电机 */
static void uart_send_failsafe(void)
{
    ctrl_payload_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.mode = MOTOR_MODE_MIT;
    /* flags 里没有 ENABLE 位 = 失能 */

    ctrl_target_t t;
    proto_payload_to_target(&pl, &t);

    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_ctrl(buf, &t, 0 /* 无 ENABLE */, MOTOR_MODE_MIT, g_fwd_seq++);

    Serial1.write(buf, CTRL_FRAME_SIZE);
    Serial1.flush();
    g_failsafe_sent++;
}

/* ================================================================== */
/* 串口: 解析 STM32 反馈帧                                            */
/* ================================================================== */

static void feed_status_byte(uint8_t b)
{
    switch (g_fb_state) {
    case 0:   /* 找帧头 */
        if (b == PROTO_STX) {
            g_fb_buf[0] = b;
            g_fb_len = 1;
            g_fb_state = 1;
        }
        break;

    case 1:   /* 收满一帧 */
        if (g_fb_len < RX_FRAME_SIZE) {
            g_fb_buf[g_fb_len++] = b;
        }
        if (g_fb_len >= RX_FRAME_SIZE) {
            status_frame_t st;
            if (proto_decode_status(g_fb_buf, RX_FRAME_SIZE, &st)) {
                portENTER_CRITICAL(&g_status_mux);
                g_last_status = st;
                g_have_status = true;
                portEXIT_CRITICAL(&g_status_mux);
                g_status_rx++;
            } else {
                g_rx_bad++;
            }
            g_fb_state = 0;
            g_fb_len = 0;
        }
        break;

    default:
        g_fb_state = 0;
        break;
    }
}

static void poll_stm32_uart(void)
{
    int avail = Serial1.available();
    while (avail-- > 0) {
        int c = Serial1.read();
        if (c < 0) break;
        feed_status_byte((uint8_t)c);
    }
}

/* ================================================================== */
/* 桥接任务                                                           */
/* ================================================================== */

static void bridge_task(void *arg)
{
    (void)arg;
    rx_item_t item;
    uint32_t last_failsafe_ms = 0;

    for (;;) {
        /* 等一帧, 最长 10ms 超时, 保证看门狗和串口轮询不被饿死 */
        if (xQueueReceive(g_rx_queue, &item, pdMS_TO_TICKS(10)) == pdTRUE) {
            ctrl_payload_t pl;
            uint8_t seq;

            /* 回调里已经校验过, 这里再解一次取出字段 (便宜且安全) */
            if (!proto_decode_ctrl(item.data, CTRL_FRAME_SIZE, &pl, &seq)) {
                g_rx_bad++;
                continue;
            }

            /* 序号查重: ESP-NOW 有重传, 旧目标不能把新目标顶回去 */
            if (g_have_seq && seq == g_last_seq) {
                g_rx_dup++;
                continue;
            }
            g_last_seq  = seq;
            g_have_seq  = true;

            g_rx_ok++;
            g_last_valid_ms = millis();
            g_link_up = true;

            if (g_failsafe_active) {
                g_failsafe_active = false;
#if BRIDGE_DEBUG
                Serial.println(F("[桥接] 主机恢复, 退出失联保护"));
#endif
            }

            uart_send_ctrl(&pl, pl.flags, pl.mode, g_fwd_seq++);
            g_fwd_ok++;
        }

        /* ---- 看门狗 ---- */
        uint32_t now = millis();
        bool stale = (uint32_t)(now - g_last_valid_ms) > BRIDGE_LINK_TIMEOUT_MS;
        if (stale) {
            if (g_link_up) {
                g_link_up = false;
#if BRIDGE_DEBUG
                Serial.printf("[桥接] !! 主机失联 %lums, 向 STM32 注入失能帧\n",
                              (unsigned long)(now - g_last_valid_ms));
#endif
            }
            if ((uint32_t)(now - last_failsafe_ms) >= BRIDGE_FAILSAFE_PERIOD_MS) {
                last_failsafe_ms = now;
                g_failsafe_active = true;
                uart_send_failsafe();
            }
        }

        /* ---- 收 STM32 反馈 ---- */
        poll_stm32_uart();

        /* ---- 周期日志 ---- */
#if BRIDGE_DEBUG
        if ((uint32_t)(now - g_last_log_ms) >= BRIDGE_LOG_PERIOD_MS) {
            g_last_log_ms = now;
            if (g_have_status) {
                Serial.printf("[桥接] %s rssi=%d | 主机 ok=%lu bad=%lu dup=%lu | STM32 yaw=%.2f pitch=%.2f "
                              "T=%uC err=%u | 转发=%lu\n",
                              g_link_up ? "在线" : "【失联】",
                              g_last_rssi,
                              (unsigned long)g_rx_ok, (unsigned long)g_rx_bad,
                              (unsigned long)g_rx_dup,
                              g_last_status.yaw_deg, g_last_status.pitch_deg,
                              g_last_status.temp_mos, g_last_status.err_code,
                              (unsigned long)g_fwd_ok);
            } else {
                Serial.printf("[桥接] %s rssi=%d | 主机 ok=%lu bad=%lu | 还没有 STM32 反馈 (检查 PA9/PA10 接线)\n",
                              g_link_up ? "在线" : "【失联】",
                              g_last_rssi,
                              (unsigned long)g_rx_ok, (unsigned long)g_rx_bad);
            }
        }
#endif
    }
}

/* ================================================================== */
/* 初始化                                                             */
/* ================================================================== */

static bool espnow_start(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.println(F("[ESP-NOW] 设置信道失败"));
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[ESP-NOW] 初始化失败"));
        return false;
    }

    /* 不用条件判断返回值: 2.x 返回 void, 3.x 返回 esp_err_t */
    esp_now_register_recv_cb(bridge_on_recv);

    /* 接收方也要把主机注册成 peer, 否则某些 core 版本收不到单播 */
#if ESPNOW_FILTER_MASTER
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, g_master_mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;
    if (!esp_now_is_peer_exist(g_master_mac)) {
        esp_now_add_peer(&peer);
    }
#endif

    Serial.printf("[ESP-NOW] 就绪 信道=%d 本机MAC=%s\n",
                  ESPNOW_CHANNEL, WiFi.macAddress().c_str());
    Serial.printf("[ESP-NOW] 把上面这个 MAC 填到主机的 ESPNOW_PEER_MAC_x 里可改成定向发送\n");
    return true;
}

/* ================================================================== */
/* 显示任务                                                           */
/* ================================================================== */

static void display_task(void *arg)
{
    (void)arg;
    for (;;) {
        display_tick(millis());
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ================================================================== */
/* 状态快照 (给 OLED 用)                                              */
/* ================================================================== */

extern "C" void bridge_get_status(bridge_status_t *out)
{
    if (out == nullptr) {
        return;
    }

    out->link_up       = g_link_up;
    out->rssi          = g_last_rssi;
    out->rx_ok         = g_rx_ok;
    out->rx_bad        = g_rx_bad;
    out->rx_dup        = g_rx_dup;
    out->fwd_ok        = g_fwd_ok;
    out->failsafe_sent = g_failsafe_sent;
    out->status_rx     = g_status_rx;
    out->uptime_ms     = millis();

    /* g_last_status 是结构体, 可能被桥接任务写到一半。
     * 短临界区拷贝一份, 避免显示读到撕裂的浮点数。 */
    portENTER_CRITICAL(&g_status_mux);
    bool have = g_have_status;
    if (have) {
        out->stm32_yaw_deg = g_last_status.yaw_deg;
        out->stm32_temp    = g_last_status.temp_mos;
        out->stm32_err     = g_last_status.err_code;
        out->stm32_state   = g_last_status.state;
    } else {
        out->stm32_yaw_deg = 0.0f;
        out->stm32_temp    = 0;
        out->stm32_err     = 0;
        out->stm32_state   = 0;
    }
    portEXIT_CRITICAL(&g_status_mux);
    out->have_stm32 = have;
}

void setup(void)
{
    Serial.begin(115200);
    delay(400);

    Serial.println();
    Serial.println(F("=============================================="));
    Serial.println(F("  靶车 · M5Stamp S3 桥接端"));
    Serial.println(F("  ESP-NOW -> UART -> STM32 -> CAN -> DM-G6220"));
    Serial.println(F("=============================================="));

    /* OLED 状态屏 (G9=SCL / G8=SDA)。没接屏时自动跳过, 不影响桥接功能 */
    if (display_init()) {
        Serial.println(F("[OLED] 128x64 屏幕就绪 (G9=SCL, G8=SDA)"));
    } else {
        Serial.println(F("[OLED] 未检测到屏幕 (不影响桥接功能)"));
    }

    /* 与 STM32 的链路 */
    Serial1.begin(LINK_BAUD, SERIAL_8N1, LINK_PIN_RXD, LINK_PIN_TXD);
    Serial.printf("[UART] Serial1 TX=G%d RX=G%d %d bps\n",
                  LINK_PIN_TXD, LINK_PIN_RXD, LINK_BAUD);
    Serial.println(F("[UART] 接线: 桥接 TX(G1) -> STM32 PA10(USART1_RX)"));
    Serial.println(F("[UART]      桥接 RX(G2) <- STM32 PA9 (USART1_TX)   并务必共地!"));

    g_rx_queue = xQueueCreate(BRIDGE_RX_QUEUE_DEPTH, sizeof(rx_item_t));
    if (!g_rx_queue) {
        Serial.println(F("[致命] 队列创建失败"));
    }

    if (!espnow_start()) {
        Serial.println(F("[致命] ESP-NOW 起不来"));
    }

    /* 桥接任务放在核心 1, 与 WiFi 任务分开, 减少抖动 */
    xTaskCreatePinnedToCore(bridge_task, "bridge", 4096, nullptr, 5, &g_bridge_task, 1);

    /* ★ 显示任务放在核心 0:
     *   软件 I2C 刷一屏要几毫秒, 放在网桥任务里会影响 ESP-NOW 的实时性。
     *   单独一个低优先级任务, 刷新慢一点也无所谓 (10Hz 足够看)。 */
    xTaskCreatePinnedToCore(display_task, "oled", 3072, nullptr, 2, nullptr, 0);

    g_last_valid_ms = millis();
    g_last_log_ms   = millis();
}

void loop(void)
{
    /* 全部工作都在 bridge_task / display_task 里, 这里只留个心跳防止看门狗复位 */
    delay(1000);
}
