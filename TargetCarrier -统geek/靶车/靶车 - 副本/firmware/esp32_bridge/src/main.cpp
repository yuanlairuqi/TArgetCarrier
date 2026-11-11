/**
 * @file    main.cpp
 * @brief   靶车 —— M5Stamp S3 桥接端 (ESP-NOW -> UART -> STM32)
 *
 *   数据流:
 *     主机 ESP-NOW  ──►  on_espnow_recv() 回调 (Wi-Fi 任务上下文: 只校验+拷贝+入队)
 *                              │  xQueueSend(..., 0)  按类型分两条队列
 *                              ▼
 *                        bridge_task (独立任务, 核心 1)
 *                              │  过期丢弃 / 序号窗口 / FAILSAFE 判定 / 原样转发
 *                              ▼
 *                        Serial1 ──► STM32 USART1 ──► CAN ──► DM-G6220
 *
 *   反向:
 *     Serial1 ◄── STM32 反馈帧 ──► lc_rx_t 成帧 ──► ESP-NOW 上行给主机
 *
 *   安全设计 (与 verification/freertos_port_20261024/DESIGN_CONTRACT.md §5 对齐):
 *     - 主机静默 >300ms -> FAILSAFE 锁存: 清空两条接收队列, 每 50ms 注入一帧
 *       flags=CTRL_FLAG_FAILSAFE 的失能帧; 期间拒绝转发任何 ENABLE 帧;
 *       只有"链路恢复 且 收到过主机明确的 ENABLE=0 帧"才退出, 退出后 STM32 仍然
 *       要求主机下发新的 ARM 令牌才会运动 (桥接不替主机做授权)。
 *     - 安全帧 (急停/失能/清错/存零点/ENABLE 边沿) 走独立队列, 每轮先清空,
 *       不会因为普通运动帧占满队列而丢失。
 *     - 命令出队超过 100ms 直接丢弃并计数: 旧目标绝不能当成新命令执行。
 *     - STM32 反馈只在新鲜 (<=300ms) 时转发; 失联后不再重发最后一帧缓存。
 *     - 初始化失败 (队列/任务/上行 peer) 进入降级安全态: 不转发 ENABLE, 持续注入失能帧。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "protocol.h"
#include "linkcore.h"
#include "app_config.h"
#include "bridge_status.h"
#include "display.h"
#include "netcfg.h"

/* 允许用 ESP_ARDUINO_VERSION_MAJOR 做条件编译; 老核心不带这个宏 */
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

/* ================================================================== */
/* 状态                                                               */
/* ================================================================== */

/* ESP-NOW 回调 -> bridge_task 的队列元素 (契约 §5.2) */
typedef struct {
    uint8_t  data[CTRL_FRAME_SIZE];  /* 原始 17 字节: 转发时不再重编码、不改 seq */
    int8_t   rssi;                   /* 2.x 回调拿不到时固定 0 (=未知, 不是"信号很好") */
    uint32_t rx_ms;                  /* 入队时刻, 用于出队时 100ms 过期判定 */
} rx_item_t;

static QueueHandle_t     s_safety_q       = nullptr;  /* 安全帧优先队列 (深度 4) */
static QueueHandle_t     s_cmd_q          = nullptr;  /* 普通运动/增益/参数队列 (深度 16) */
static TaskHandle_t      g_bridge_task    = nullptr;

/* 收包统计: 回调 (Wi-Fi 任务) 与桥接任务/显示任务都会读写, 统一用 volatile */
static volatile uint32_t g_rx_ok          = 0;   /* 通过序号窗口的有效 CTRL 帧 */
static volatile uint32_t g_rx_bad         = 0;   /* 长度/CRC/类型校验失败的帧 */
static volatile uint32_t g_rx_dup         = 0;   /* 重复/乱序/迟到 (lc_seq_gate 拒绝) */
static volatile uint32_t g_rx_drop        = 0;   /* 队列满丢弃 (安全队列为"顶掉最旧") */
static volatile uint32_t g_cmd_expired    = 0;   /* 出队时超过 CTRL_CMD_MAX_AGE_MS 丢弃 */
static volatile uint32_t g_fwd_ok         = 0;   /* 成功转发给 STM32 的 CTRL 帧 */
static volatile uint32_t g_gains_fwd      = 0;   /* 成功转发的增益帧 */
static volatile uint32_t g_param_fwd      = 0;   /* 成功转发的参数帧 */
static volatile uint32_t g_uart_tx_fail   = 0;   /* Serial1.write 短写/失败 */
static volatile uint32_t g_arm_reject     = 0;   /* FAILSAFE/降级期间被拒绝转发的 ENABLE 帧 */
static volatile uint32_t g_failsafe_sent  = 0;   /* 注入失能帧次数 (含写失败, 写失败另计) */
static volatile uint32_t g_rx_overflow    = 0;   /* 串口积压/半帧 -> lc_rx_overflow 次数 */
static volatile uint32_t g_uplink_fail    = 0;   /* ESP-NOW 上行发送失败 / peer 未就绪丢弃 */
static volatile uint32_t g_uplink_timeout = 0;   /* 上行在途回调超时次数 */
static volatile uint32_t g_status_rx      = 0;   /* 收到 STM32 STATUS 帧数 */
static volatile uint32_t g_uart_raw       = 0;   /* 从 STM32 收到的原始字节数 (诊断: 0=线没通) */
static volatile uint16_t g_can_fail       = 0xFFFFu; /* STM32 报上来的 CAN 失败计数 */
static volatile int8_t   g_last_rssi      = 0;   /* 最近一次 ESP-NOW 的 RSSI; 0 = 未知 */
static volatile uint32_t g_last_host_ms   = 0;   /* 最近一次收到主机合法帧的时刻 (回调里更新) */
static volatile bool     g_link_up        = false;
static volatile bool     g_failsafe_active = false; /* FAILSAFE 锁存 */
static volatile bool     g_motion_blocked = false;  /* 对外: 运动被桥接锁住 */
static volatile bool     s_bridge_degraded = false; /* 初始化失败: 降级安全态 */
static volatile bool     s_espnow_ok      = false; /* esp_now_init 成功过 (失败时禁止再调 ESP-NOW API) */
static volatile bool     g_uplink_ready   = false;  /* 上行 ESP-NOW peer 就绪 */
static volatile bool     g_have_stm       = false;  /* 本次上电后收到过 STM32 帧 */

/* 只在 bridge_task 里用的状态 */
static lc_seq_gate_t     g_seq;             /* CTRL 序号窗口: 去重/乱序/迟到/回绕 */
static lc_rx_t           g_stm_rx;          /* STM32 字节流成帧器 (替代手写状态机) */
static lc_fresh_t        g_stm_fb;          /* STM32 反馈新鲜度 */
static lc_txstats_t      g_up_tx;           /* 上行 ESP-NOW 提交/回调/确认统计 */
static bool              s_saw_disarm       = false; /* 上电/进入 FAILSAFE 之后收到过主机 ENABLE=0 帧
                                                      * (运动许可门 + FAILSAFE 退出条件) */
static uint32_t          g_stm_last_byte_ms = 0;     /* 串口最后一次收到字节的时刻 */
static uint8_t           g_inject_seq       = 0;     /* 注入失能帧自己的 seq */
static uint8_t           g_br_status_seq    = 0;     /* 桥接新鲜度帧的 seq */
static uint32_t          g_stm_up_ms        = 0;     /* 上次 STATUS 上行时刻 */
static uint32_t          g_peer_retry_ms    = 0;     /* 上次上行 peer 注册尝试时刻 */

/* 只在 ESP-NOW 接收回调 (Wi-Fi 任务上下文) 里用的状态 */
static bool              s_last_enable      = false; /* 上一帧的 ENABLE 电平, 用于识别边沿 */

/* 状态回传用广播: 主机端会收到, 也避免依赖"主机 MAC 白名单"是否开启 */
static const uint8_t     g_bcast_mac[6]     = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static uint32_t          g_last_log_ms      = 0;

/* 保护 g_last_status / g_have_status / g_stm_fb (桥接任务写, 显示任务读) */
static portMUX_TYPE      g_status_mux       = portMUX_INITIALIZER_UNLOCKED;
/* 保护 g_up_tx (ESP-NOW 发送回调在 Wi-Fi 任务上下文, 与桥接任务并发) */
static portMUX_TYPE      g_tx_mux           = portMUX_INITIALIZER_UNLOCKED;

/* OLED 显示用的最近 STATUS 缓存。★ 只用于显示, 绝不用于重发:
 * 桥接失联后如果继续把缓存帧当"实时状态"发给主机, 主机会误判 STM32 还在线。 */
static status_frame_t    g_last_status;
static bool              g_have_status      = false;

/* 主机 MAC 白名单。只在 ESPNOW_FILTER_MASTER=1 时才会被用到,
 * 默认为 0(不过滤), 所以这里加 unused 属性避免 -Wunused 警告。 */
#if ESPNOW_FILTER_MASTER
static uint8_t g_master_mac[6] = {
    ESPNOW_MASTER_MAC_0, ESPNOW_MASTER_MAC_1, ESPNOW_MASTER_MAC_2,
    ESPNOW_MASTER_MAC_3, ESPNOW_MASTER_MAC_4, ESPNOW_MASTER_MAC_5
};
#endif

/* ================================================================== */
/* ESP-NOW 接收回调 —— 运行在 Wi-Fi 任务上下文 (不是中断!)             */
/* ================================================================== */
/* 因此:
 *   - 只能用非阻塞的 xQueueSend(..., 0); 不能用"中断专用"的那套入队/让出
 *     API —— 旧实现把回调当 ISR 处理, 在 Wi-Fi 任务里调中断 API 是错的;
 *   - 也绝不能阻塞等待或写串口, 否则整个 Wi-Fi 协议栈都会被拖慢。 */

/* 回调入队。普通帧满 -> 丢新帧计数; 安全帧满 -> 先出队最旧一条再入队,
 * 保证"停止/急停/失能"这类命令不会因为队列满而静默丢失 (契约 §5.2)。 */
static bool enqueue_item(QueueHandle_t q, const rx_item_t *it, bool safety)
{
    if (q == nullptr) {
        return false;
    }
    if (xQueueSend(q, it, 0) == pdTRUE) {
        return true;
    }

    if (!safety) {
        g_rx_drop++;
        return false;
    }

    /* 安全帧: 顶掉最旧的一条。只有回调一个生产者, 这里的"先收再送"不会
     * 与其它生产者竞争; 消费者(桥接任务)最多提前腾出位置, 不影响正确性。 */
    rx_item_t oldest;
    (void)xQueueReceive(q, &oldest, 0);
    g_rx_drop++;
    if (xQueueSend(q, it, 0) != pdTRUE) {
        g_rx_drop++;
        return false;
    }
    return true;
}

/* 判断一帧 CTRL 是否属于"安全命令": 急停/失能注入/清错/存零点, 或 ENABLE 边沿。
 * ENABLE 从 0->1 (开始能运动) 和 1->0 (失去运动许可) 都决定"电机能不能动",
 * 所以两条边沿都走安全队列, 不允许排在普通帧后面等到过期。 */
static bool ctrl_is_safety(uint16_t flags)
{
    const uint16_t mask = (uint16_t)(CTRL_FLAG_ESTOP | CTRL_FLAG_FAILSAFE |
                                     CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO);
    const bool en   = (flags & CTRL_FLAG_ENABLE) != 0u;
    const bool edge = (en != s_last_enable);
    s_last_enable = en;
    return ((flags & mask) != 0u) || edge;
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
/* Arduino-ESP32 3.x 使用 esp_now_recv_info_t, 里面带 RSSI */
static void on_espnow_recv(const esp_now_recv_info_t *info, const uint8_t *data, int len)
{
    const uint8_t *mac = info->src_addr;
    int8_t rssi = (int8_t)(info->rx_ctrl != nullptr ? info->rx_ctrl->rssi : 0);
    const bool have_rssi = true;
#else
/* Arduino-ESP32 2.x: 接收回调签名里没有 RSSI 参数, 这里固定 0 (=未知)。
 * 只有 STA 连上路由器时才在 bridge_task 里用 esp_wifi_sta_get_ap_info() 补一个
 * 近似值。★ 0 表示"未知", 不得当作链路质量结论。 */
static void on_espnow_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    int8_t rssi = 0;
    const bool have_rssi = false;
#endif
    const uint32_t now = millis();

    /* 白名单关闭时 mac 不参与判断, 这里显式忽略, 避免 -Wunused-parameter */
    (void)mac;

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

    const uint8_t type = data[2];
    bool safety = false;

    /* 回调只做"长度/CRC 预校验 + 拷贝 + 入队": 坏帧根本不进队列, 主任务不用白跑 */
    if (type == (uint8_t)PROTO_MSG_CTRL) {
        ctrl_payload_t pl;
        uint8_t seq;
        if (!proto_decode_ctrl(data, (uint16_t)len, &pl, &seq)) {
            g_rx_bad++;
            return;
        }
        safety = ctrl_is_safety(pl.flags);
    } else if (type == (uint8_t)PROTO_MSG_GAINS) {
        float kp = 0.0f, kd = 0.0f;
        if (!proto_decode_gains(data, (uint16_t)len, &kp, &kd)) {
            g_rx_bad++;
            return;
        }
    } else if (type == (uint8_t)PROTO_MSG_PARAM) {
        uint8_t op, rid, val[4];
        if (!proto_decode_param(data, (uint16_t)len, &op, &rid, val)) {
            g_rx_bad++;
            return;
        }
    } else {
        /* 未知类型 (例如主机侧今后新增的帧): 丢弃并计数, 不入队 */
        g_rx_bad++;
        return;
    }

    /* 主机是否在线看"收到过任何合法帧", 与序号窗口的"是不是新命令"解耦:
     * ESP-NOW 重传同一个序号也证明主机还活着, 不应因此触发失联保护。 */
    g_last_host_ms = now;
    if (have_rssi) {
        g_last_rssi = rssi;
    }

    rx_item_t item;
    memcpy(item.data, data, CTRL_FRAME_SIZE);
    item.rssi = rssi;
    item.rx_ms = now;
    (void)enqueue_item(safety ? s_safety_q : s_cmd_q, &item, safety);
}

/* ================================================================== */
/* 串口: 向 STM32 写帧                                                */
/* ================================================================== */

/* 原样写一帧给 STM32。返回 true = 全部字节写入。
 * 必须检查 Serial1.write 的返回值: 短写不会报错, 却会在 STM32 侧造成
 * 半帧/重同步, 等于静默丢命令; 这里计 uart_tx_fail 便于现场判断。 */
static bool uart_send_raw(const uint8_t *frame, uint8_t len)
{
    const size_t n = Serial1.write(frame, len);
    if (n > 0u) {
        /* 只有确实写进去了才 flush (写 0 字节时 flush 没有意义)。
         * 短写时也 flush: 让 STM32 尽快看到半帧并重新同步, 而不是留在缓冲区里
         * 和下一帧拼成假帧。 */
        Serial1.flush();     /* 17 字节 @921600 ≈ 185us, 不会明显阻塞 */
    }
    if (n != (size_t)len) {
        g_uart_tx_fail++;
        return false;
    }
    return true;
}

/* 失联保护: 注入一帧"flags 只含 FAILSAFE 位"的失能帧。
 * 没有 ENABLE/ARM/ESTOP, ID/角度/速率全 0, seq 由桥接自己自增;
 * STM32 对带 FAILSAFE 的帧必须绕过序号判定并立即撤销授权 (契约 §1.1)。 */
static void inject_failsafe_frame(void)
{
    ctrl_target_t target;
    memset(&target, 0, sizeof(target));

    uint8_t buf[CTRL_FRAME_SIZE];
    proto_encode_ctrl(buf, &target, CTRL_FLAG_FAILSAFE, MOTOR_MODE_MIT, g_inject_seq++);
    (void)uart_send_raw(buf, CTRL_FRAME_SIZE);
    g_failsafe_sent++;   /* 统计注入次数; 写失败另记 uart_tx_fail */
}

/* ================================================================== */
/* ESP-NOW 上行 (桥接 -> 主机)                                        */
/* ================================================================== */

/* 上行 peer (广播) 注册: 旧实现不看 esp_now_add_peer 返回值, 注册失败也把
 * UPLINK_READY 置 1, 结果主机一帧上行都收不到却以为桥接正常。
 * 现在检查返回值, 失败 1s 后重试, 成功才置 UPLINK_READY。 */
static bool uplink_peer_ensure(uint32_t now)
{
    if (!s_espnow_ok) {
        return false;    /* esp_now_init 都没成功: 不能再调 ESP-NOW API */
    }
    if (g_uplink_ready) {
        return true;
    }
    if (g_peer_retry_ms != 0u &&
        (uint32_t)(now - g_peer_retry_ms) < BRIDGE_PEER_RETRY_MS) {
        return false;    /* 限频: 1s 内不重复尝试 */
    }
    g_peer_retry_ms = now;

    if (esp_now_is_peer_exist(g_bcast_mac)) {
        g_uplink_ready = true;
        Serial.println(F("[ESP-NOW] 上行广播 peer 已存在, 上行就绪"));
        return true;
    }

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, g_bcast_mac, 6);
    peer.channel = 0;            /* 0 = 跟随当前信道 */
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;

    const esp_err_t rc = esp_now_add_peer(&peer);
    if (rc != ESP_OK) {
        Serial.printf("[ESP-NOW] 上行 peer 注册失败 rc=%d, %lums 后重试\n",
                      (int)rc, (unsigned long)BRIDGE_PEER_RETRY_MS);
        return false;
    }
    g_uplink_ready = true;
    Serial.println(F("[ESP-NOW] 上行广播 peer 注册成功, 上行就绪"));
    return true;
}

/* ESP-NOW 发送回调 (Wi-Fi 任务上下文): 只把结果记到 lc_txstats, 不做耗时操作。
 * g_up_tx 与桥接任务共享, 用极短临界区保护在途表/计数, 避免计数撕裂。 */
static void on_uplink_sent(const uint8_t *mac, esp_now_send_status_t status)
{
    (void)mac;
    const uint32_t now = millis();
    portENTER_CRITICAL(&g_tx_mux);
    lc_txstats_cb(&g_up_tx, status == ESP_NOW_SEND_SUCCESS, now);
    portEXIT_CRITICAL(&g_tx_mux);
}

/* 上行发送 (桥接 -> 主机)。契约 §5.7: 上一帧没收到发送回调前不再提交下一帧,
 * 250ms 超时后由 lc_txstats_expire 放行 (超时也计数), 不无限等待。
 * 注意: 这里"丢帧"是正常设计 —— 上行都是周期性的诊断数据, 下一周期会再来,
 * 绝不缓存旧帧补发 (旧数据不能当新数据用)。 */
static bool uplink_send(uint32_t now, const uint8_t *buf, uint8_t len)
{
    if (!uplink_peer_ensure(now)) {
        g_uplink_fail++;     /* peer 未就绪: 本帧丢弃并计数 */
        return false;
    }

    portENTER_CRITICAL(&g_tx_mux);
    const bool busy = (lc_txstats_inflight(&g_up_tx) > 0u);
    portEXIT_CRITICAL(&g_tx_mux);
    if (busy) {
        return false;        /* 在途未回调: 不提交下一帧 */
    }

    /* Reserve before send: the Wi-Fi callback can run before esp_now_send returns. */
    portENTER_CRITICAL(&g_tx_mux);
    (void)lc_txstats_submit(&g_up_tx, LC_TX_UPLINK, (uint16_t)len, now, true);
    portEXIT_CRITICAL(&g_tx_mux);
    const esp_err_t rc = esp_now_send(g_bcast_mac, buf, len);
    if (rc != ESP_OK) {
        portENTER_CRITICAL(&g_tx_mux);
        g_up_tx.count = 0;
        g_up_tx.submit_ok[LC_TX_UPLINK]--;
        g_up_tx.submit_fail[LC_TX_UPLINK]++;
        portEXIT_CRITICAL(&g_tx_mux);
    }

    if (rc != ESP_OK) {
        g_uplink_fail++;
        return false;
    }
    return true;
}

/* 把 STM32 STATUS 转发给主机 —— 只在"刚收到且新鲜"时转发, 不做缓存重发。
 * 注意 p[4] 的 RSSI 按 protocol.h 约定由桥接填 (ESP-NOW 上行链路质量),
 * 因此这里重新编码而不是原样转发; 控制/增益/参数帧才要求原样转发。 */
static void uplink_stm_status(const status_frame_t *st, uint32_t now)
{
    bool have = false;
    const uint32_t age = lc_fresh_age(&g_stm_fb, now, &have);
    if (!have || age > BRIDGE_STM_FRESH_MS) {
        return;     /* 不新鲜: 不转发, 也绝不重发缓存 */
    }
    /* 上行只有一条在途, STM32 是 50Hz 状态帧: 限制到 10Hz, 把时隙留给
     * 桥接新鲜度帧 (否则主机会因为收不到新鲜度帧而误判桥接失联)。 */
    if (g_stm_up_ms != 0u &&
        (uint32_t)(now - g_stm_up_ms) < BRIDGE_STM_UP_PERIOD_MS) {
        return;
    }

    const int8_t r = g_last_rssi;
    const uint8_t rssi_abs = (uint8_t)((r < 0) ? -r : r);

    uint8_t up[STATUS_FRAME_SIZE];
    proto_encode_status(up, st->seq, rssi_abs, st->state,
                        st->yaw_deg, st->pitch_deg, st->temp_mos, st->err_code);
    if (uplink_send(now, up, STATUS_FRAME_SIZE)) {
        g_stm_up_ms = now;
    }
}

/* ================================================================== */
/* 串口: 解析 STM32 反馈帧 (lc_rx_t)                                  */
/* ================================================================== */

static void handle_stm_frame(lc_rx_kind_t kind, const uint8_t *frame, uint32_t now)
{
    switch (kind) {
    case LC_RX_STATUS: {
        status_frame_t st;
        if (!proto_decode_status(frame, STATUS_FRAME_SIZE, &st)) {
            g_rx_bad++;
            return;
        }
        /* 只有 STATUS 帧才代表"反馈新鲜": 电机帧/参数回包另算。
         * 缓存 + 新鲜度必须在同一个临界区里更新, 否则显示任务可能拼出
         * "新年龄 + 旧状态"的组合。 */
        portENTER_CRITICAL(&g_status_mux);
        g_last_status = st;
        g_have_status = true;
        lc_fresh_kick(&g_stm_fb, now);
        portEXIT_CRITICAL(&g_status_mux);

        g_status_rx++;
        g_have_stm = true;
        uplink_stm_status(&st, now);
        break;
    }
    case LC_RX_MOTOR: {
        float pos = 0.0f, vel = 0.0f, tau = 0.0f;
        uint16_t can_fail = 0xFFFFu;
        uint8_t safety = 0, cmd_drops = 0, arm_echo = 0;
        if (proto_decode_motor_ext2(frame, CTRL_FRAME_SIZE, &pos, &vel, &tau,
                                    &can_fail, &safety, &cmd_drops, &arm_echo)) {
            g_can_fail = can_fail;
            /* safety/cmd_drops/arm_echo 是给主机显示/判定的, 桥接只透传 */
        }
        g_have_stm = true;
        /* 电机实时量: 诊断数据, 原样上行 (不重编码) */
        (void)uplink_send(now, frame, CTRL_FRAME_SIZE);
        break;
    }
    case LC_RX_PARAM_REPLY:
        g_have_stm = true;
        /* 参数回包: 原样上行 (不重编码) */
        (void)uplink_send(now, frame, CTRL_FRAME_SIZE);
        break;
    default:
        /* 其它类型不应由 STM32 发来, 忽略 */
        break;
    }
}

static void poll_stm32_uart(uint32_t now)
{
    const int avail = Serial1.available();

    if (avail <= 0) {
        /* 半帧超时: 已收到帧头但后续字节长时间不到 (干扰/线松/对端复位),
         * 丢掉这半帧, 等下一次帧头重新同步, 否则会把下一帧的帧头当成载荷。 */
        if (g_stm_rx.len > 0u &&
            lc_elapsed_ms(now, g_stm_last_byte_ms) > BRIDGE_UART_FRAME_GAP_MS) {
            lc_rx_overflow(&g_stm_rx);
            g_rx_overflow++;
        }
        return;
    }

    /* 积压超过阈值: 本轮消费已经明显落后于串口速率, 帧中间的内容多半已经坏了,
     * 丢掉当前半帧让成帧器重新找帧头, 并计数 (契约 §5.3)。 */
    if (avail > BRIDGE_UART_BACKLOG_MAX) {
        lc_rx_overflow(&g_stm_rx);
        g_rx_overflow++;
    }

    uint8_t frame[CTRL_FRAME_SIZE];
    int left = avail;
    while (left-- > 0) {
        const int c = Serial1.read();
        if (c < 0) {
            break;
        }
        g_uart_raw++;                 /* ★ 诊断: PA9 上到底有没有电平进来 */
        g_stm_last_byte_ms = now;
        const lc_rx_kind_t kind = lc_rx_feed(&g_stm_rx, (uint8_t)c, now, frame);
        if (kind != LC_RX_NONE) {
            handle_stm_frame(kind, frame, now);
        }
    }
}

/* ================================================================== */
/* 桥接 -> 主机 新鲜度帧                                              */
/* ================================================================== */

static void send_bridge_status(uint32_t now)
{
    bool have_age = false;
    const uint32_t age = lc_fresh_age(&g_stm_fb, now, &have_age);
    const bool fresh = have_age && age <= BRIDGE_STM_FRESH_MS;

    /* 以 10ms 为单位饱和到 255 (2550ms); 从未收到过反馈时填 0xFF = "无数据" */
    uint8_t age10 = 0xFFu;
    if (have_age) {
        const uint32_t q = age / 10u;
        age10 = (uint8_t)(q > 0xFFu ? 0xFFu : q);
    }

    status_frame_t st;
    bool have_status = false;
    portENTER_CRITICAL(&g_status_mux);
    st = g_last_status;
    have_status = g_have_status;
    portEXIT_CRITICAL(&g_status_mux);

    uint8_t flags = 0;
    if (fresh)                            { flags |= PROTO_BR_FLAG_STM_FRESH; }
    if (g_failsafe_active)                { flags |= PROTO_BR_FLAG_FAILSAFE; }
    if (g_have_stm)                       { flags |= PROTO_BR_FLAG_HAVE_STM; }
    if (g_motion_blocked)                 { flags |= PROTO_BR_FLAG_MOTION_BLOCKED; }
    if (g_uplink_ready)                   { flags |= PROTO_BR_FLAG_UPLINK_READY; }
    if (g_rx_overflow > 0u)               { flags |= PROTO_BR_FLAG_RX_OVERFLOW; }
    if (have_status && st.state == (uint8_t)LINK_STATE_ESTOP) {
        flags |= PROTO_BR_FLAG_STM_ESTOP;
    }

    uint8_t buf[BRIDGE_STATUS_FRAME_SIZE];
    /* 计数一律给低 8 位, 主机只看趋势; 完整计数在本机日志里 */
    proto_encode_bridge_status(buf, g_br_status_seq++, flags, age10,
                               have_status ? st.state : 0,
                               have_status ? st.err_code : 0,
                               (uint8_t)(g_rx_ok & 0xFFu),
                               (uint8_t)(g_rx_dup & 0xFFu),
                               (uint8_t)((g_rx_drop + g_cmd_expired) & 0xFFu),
                               (uint8_t)(g_fwd_ok & 0xFFu),
                               (uint8_t)(g_failsafe_sent & 0xFFu),
                               (uint8_t)(g_uplink_fail & 0xFFu),
                               (uint8_t)(g_arm_reject & 0xFFu));
    (void)uplink_send(now, buf, BRIDGE_STATUS_FRAME_SIZE);
}

/* ================================================================== */
/* 主机命令处理                                                       */
/* ================================================================== */

static void handle_host_frame(const rx_item_t &item, uint32_t now)
{
    /* 1) 过期命令: 队列积压时旧目标绝不能当成新命令执行 (契约 §2: 100ms)。
     *    安全命令同样适用 —— STM32 侧出队也有同样的 100ms 规则, 拖太久的
     *    "停止"已经由桥接自己的 FAILSAFE 注入兜底。 */
    if (lc_age_exceeds(now, item.rx_ms, CTRL_CMD_MAX_AGE_MS)) {
        g_cmd_expired++;
        return;
    }

    const uint8_t type = item.data[2];

    /* 2) 增益/参数帧: 原样转发 17 字节 (不重编码、不改 seq) */
    if (type == (uint8_t)PROTO_MSG_GAINS) {
        if (uart_send_raw(item.data, GAINS_FRAME_SIZE)) {
            g_gains_fwd++;
        }
        return;
    }
    if (type == (uint8_t)PROTO_MSG_PARAM) {
        if (uart_send_raw(item.data, CTRL_FRAME_SIZE)) {
            g_param_fwd++;
        }
        return;
    }

    /* 3) CTRL 帧: 用 lc_seq_gate 做 8 bit 序号窗口
     *    (重复/乱序/迟到/回绕都比旧实现的"相邻相等"正确) */
    ctrl_payload_t pl;
    uint8_t seq;
    if (!proto_decode_ctrl(item.data, CTRL_FRAME_SIZE, &pl, &seq)) {
        g_rx_bad++;
        return;
    }
    if (!lc_seq_accept(&g_seq, seq)) {
        g_rx_dup++;
        return;
    }
    g_rx_ok++;

    const bool want_enable = (pl.flags & CTRL_FLAG_ENABLE) != 0u;

    /* 记录"主机明确失能"事件: 这是 FAILSAFE 的退出条件之一 (契约 §5.5)。
     * 在进入 FAILSAFE 时会被清掉, 所以必须看到本次事件之后的失能帧才算数。 */
    if (!want_enable) {
        s_saw_disarm = true;
    }

    /* 4) 运动许可门: 下面三种情况一律不转发 ENABLE 帧 ——
     *    a) FAILSAFE 锁存 (主机刚恢复时可能还带着"断联前的旧使能", 契约 §5.5);
     *    b) 初始化降级安全态;
     *    c) 本次上电/本次 FAILSAFE 事件之后, 还没见过主机明确的 ENABLE=0 帧。
     *    为什么 (c) 上电也要关门: 桥接 (甚至 STM32) 刚复位时, 主机可能仍在发
     *    断联前的 ENABLE|ARM|旧令牌; STM32 复位后 last_token 已清零, 会把旧令牌
     *    当成一次新的明确授权 -> 复位后自动恢复运动 (违反契约 §0.5.1)。
     *    主机侧链路超时进入 SAFE 后会先发 ENABLE=0, 门随后打开, 不影响正常操作。 */
    if (want_enable && (g_failsafe_active || s_bridge_degraded || !s_saw_disarm)) {
        g_arm_reject++;
        return;
    }

    /* 5) 原样转发 17 字节: 不重编码、不改 seq、不改 flags (含 ARM 令牌) */
    if (uart_send_raw(item.data, CTRL_FRAME_SIZE)) {
        g_fwd_ok++;
    }
}

/* ================================================================== */
/* 桥接任务                                                           */
/* ================================================================== */

#if ESP_ARDUINO_VERSION_MAJOR < 3
/* Arduino-ESP32 2.x 的 ESP-NOW 回调不带 RSSI, 只能退而求其次:
 * 仅在 STA 已连上路由器时读一次 AP 信息作为近似值; 没连路由器时保持 0。
 * ★ 0 表示"未知", 不是"信号很好" —— 不要拿它当链路质量结论。 */
static void poll_rssi_sta(uint32_t now)
{
    static uint32_t s_ms = 0;
    if ((uint32_t)(now - s_ms) < 1000u) {
        return;
    }
    s_ms = now;
    if (WiFi.status() != WL_CONNECTED) {
        return;
    }
    wifi_ap_record_t ap;
    memset(&ap, 0, sizeof(ap));
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        g_last_rssi = (int8_t)ap.rssi;
    }
}
#endif

static void bridge_task(void *arg)
{
    (void)arg;
    rx_item_t item;
    uint32_t last_failsafe_ms = 0;
    uint32_t last_up_ms       = 0;

    for (;;) {
        const uint32_t now = millis();

#if ESP_ARDUINO_VERSION_MAJOR < 3
        poll_rssi_sta(now);
#endif

        /* ---- 1. 先清空安全队列 ----
         * 急停/失能/清错/存零点/ENABLE 边沿优先于任何普通运动帧。
         * 队列为 nullptr 说明初始化失败, 已在降级安全态: 不碰空队列。 */
        while (s_safety_q != nullptr && xQueueReceive(s_safety_q, &item, 0) == pdTRUE) {
            handle_host_frame(item, now);
        }

        /* ---- 2. 普通队列: 每轮限量, 防止持续满载时把下面的周期安全动作饿死 ---- */
        if (s_cmd_q != nullptr) {
            uint8_t budget = BRIDGE_CMD_BATCH;
            while (budget-- > 0u && xQueueReceive(s_cmd_q, &item, 0) == pdTRUE) {
                handle_host_frame(item, now);
            }
        }

        /* ---- 3. 收 STM32 反馈: 成帧/校验交给 lc_rx_t, 溢出自动重新同步 ---- */
        poll_stm32_uart(now);

        /* ---- 4. 上行在途超时: 回调可能永远不来, 250ms 后放行并计超时 ---- */
        portENTER_CRITICAL(&g_tx_mux);
        const uint32_t expired = lc_txstats_expire(&g_up_tx, now, ESPNOW_CB_TIMEOUT_MS);
        portEXIT_CRITICAL(&g_tx_mux);
        if (expired > 0u) {
            g_uplink_timeout += expired;
            /* 刚放行就立刻补发一帧新鲜度帧: 否则要再等 100ms 周期, 主机可能
             * 因为超过 300ms 收不到桥接帧而误判失联。 */
            last_up_ms = 0;
        }

        /* ---- 5. FAILSAFE 状态机 ---- */
        const bool stale = lc_elapsed_ms(now, g_last_host_ms) > BRIDGE_MASTER_TIMEOUT_MS;
        g_link_up = !stale;

        if (stale && !g_failsafe_active) {
            /* 进入: 锁存 + 清空两条队列 (队列里的旧命令已经不可信),
             * 要求本次事件之后收到主机明确的 ENABLE=0 帧才可能退出。 */
            g_failsafe_active = true;
            s_saw_disarm = false;
            last_failsafe_ms = 0;       /* 立刻注入第一帧, 不等 50ms */
            if (s_safety_q != nullptr) {
                (void)xQueueReset(s_safety_q);
            }
            if (s_cmd_q != nullptr) {
                (void)xQueueReset(s_cmd_q);
            }
#if BRIDGE_DEBUG
            Serial.printf("[桥接] !! 主机静默 >%lums: 进入 FAILSAFE, 清空接收队列, 每 %lums 注入失能帧\n",
                          (unsigned long)BRIDGE_MASTER_TIMEOUT_MS,
                          (unsigned long)BRIDGE_FAILSAFE_PERIOD_MS);
#endif
        } else if (g_failsafe_active && !stale && s_saw_disarm) {
            /* 退出: 链路恢复 + 主机明确失能。退出只是解除桥接侧的锁,
             * 运动仍需主机下发新的 ARM 令牌, STM32 才会使能。 */
            g_failsafe_active = false;
#if BRIDGE_DEBUG
            Serial.println(F("[桥接] 链路恢复且收到主机 ENABLE=0: 退出 FAILSAFE (使能仍需主机新 ARM 令牌)"));
#endif
        }
        g_motion_blocked = g_failsafe_active || s_bridge_degraded;

        /* ---- 6. 失能帧注入: FAILSAFE 锁存或初始化降级期间每 50ms 一帧 ---- */
        if ((g_failsafe_active || s_bridge_degraded) &&
            (uint32_t)(now - last_failsafe_ms) >= BRIDGE_FAILSAFE_PERIOD_MS) {
            last_failsafe_ms = now;
            inject_failsafe_frame();
        }

        /* ---- 7. 每 100ms 上行桥接新鲜度帧 (STM32 真实年龄/状态/计数) ---- */
        if ((uint32_t)(now - last_up_ms) >= BRIDGE_UPLINK_PERIOD_MS) {
            last_up_ms = now;
            send_bridge_status(now);
        }

        /* ---- 8. 周期日志 ---- */
#if BRIDGE_DEBUG
        if ((uint32_t)(now - g_last_log_ms) >= BRIDGE_LOG_PERIOD_MS) {
            g_last_log_ms = now;

            bool have_age = false;
            const uint32_t age = lc_fresh_age(&g_stm_fb, now, &have_age);
            const bool fresh = have_age && age <= BRIDGE_STM_FRESH_MS;

            status_frame_t st;
            portENTER_CRITICAL(&g_status_mux);
            st = g_last_status;
            portEXIT_CRITICAL(&g_status_mux);

            if (g_have_stm) {
                Serial.printf("[桥接] %s%s rssi=%d | 主机 ok=%lu bad=%lu dup=%lu drop=%lu xp=%lu | "
                              "STM %s age=%lums yaw=%.2f T=%uC err=%u state=%u | "
                              "转发=%lu G=%lu P=%lu txerr=%lu | fs=%lu rej=%lu ovf=%lu | "
                              "上行fail=%lu 超时=%lu CF=%u\n",
                              g_link_up ? "在线" : "【失联】",
                              g_failsafe_active ? "【FAILSAFE】" : "",
                              (int)g_last_rssi,
                              (unsigned long)g_rx_ok, (unsigned long)g_rx_bad,
                              (unsigned long)g_rx_dup, (unsigned long)g_rx_drop,
                              (unsigned long)g_cmd_expired,
                              fresh ? "新鲜" : "过期", (unsigned long)age,
                              st.yaw_deg, (unsigned)st.temp_mos,
                              (unsigned)st.err_code, (unsigned)st.state,
                              (unsigned long)g_fwd_ok, (unsigned long)g_gains_fwd,
                              (unsigned long)g_param_fwd, (unsigned long)g_uart_tx_fail,
                              (unsigned long)g_failsafe_sent, (unsigned long)g_arm_reject,
                              (unsigned long)g_rx_overflow,
                              (unsigned long)g_uplink_fail, (unsigned long)g_uplink_timeout,
                              (unsigned)g_can_fail);
            } else {
                Serial.printf("[桥接] %s%s rssi=%d | 主机 ok=%lu bad=%lu dup=%lu drop=%lu xp=%lu | "
                              "还没有 STM32 反馈 (检查 PA9/PA10 接线与共地) | "
                              "转发=%lu G=%lu P=%lu txerr=%lu | fs=%lu rej=%lu ovf=%lu | "
                              "上行fail=%lu 超时=%lu\n",
                              g_link_up ? "在线" : "【失联】",
                              g_failsafe_active ? "【FAILSAFE】" : "",
                              (int)g_last_rssi,
                              (unsigned long)g_rx_ok, (unsigned long)g_rx_bad,
                              (unsigned long)g_rx_dup, (unsigned long)g_rx_drop,
                              (unsigned long)g_cmd_expired,
                              (unsigned long)g_fwd_ok, (unsigned long)g_gains_fwd,
                              (unsigned long)g_param_fwd, (unsigned long)g_uart_tx_fail,
                              (unsigned long)g_failsafe_sent, (unsigned long)g_arm_reject,
                              (unsigned long)g_rx_overflow,
                              (unsigned long)g_uplink_fail, (unsigned long)g_uplink_timeout);
            }
        }
#endif

        /* 固定 2ms 让出 CPU: Wi-Fi/显示任务还要跑, 不能把 CPU 占满;
         * 2ms 对 50Hz 控制帧、50ms 注入、100ms 上行都足够精确。
         * 注意: pdMS_TO_TICKS 在 100Hz 节拍下会把 2ms 取整成 0, 而
         * vTaskDelay(0) 只是让出、不睡眠, 会退化成忙循环 —— 所以至少睡 1 tick。 */
        const TickType_t tick = pdMS_TO_TICKS(BRIDGE_TASK_TICK_MS);
        vTaskDelay(tick > 0u ? tick : 1u);
    }
}

/* ================================================================== */
/* 初始化                                                             */
/* ================================================================== */

static bool espnow_start(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    /* ★ 有 WiFi 账号就优先连家里路由器: ESP-NOW 的信道会跟着路由器走,
     *   主机那边同理, 两端自动对齐 (比写死信道 6 更省心)。
     *   没配/连不上就退回信道 6, 和主机热点同信道。 */
    uint8_t channel = ESPNOW_CHANNEL;

    char ssid[33];
    char pass[65];
    if (netcfg_load(ssid, sizeof(ssid), pass, sizeof(pass))) {
        Serial.printf("[WiFi] 正在连接 \"%s\" ...\n", ssid);
        WiFi.begin(ssid, pass);
        const uint32_t t0 = millis();
        while (WiFi.status() != WL_CONNECTED && (millis() - t0) < WIFI_CONNECT_TIMEOUT_MS) {
            delay(100);
        }
    }

    if (WiFi.status() == WL_CONNECTED) {
        channel = (uint8_t)WiFi.channel();
        Serial.printf("[WiFi] 已连上 \"%s\", IP=%s, 信道=%d\n",
                      ssid, WiFi.localIP().toString().c_str(), channel);
    } else {
        Serial.println(F("[WiFi] 没连家里 WiFi, 用固定信道 6 (和主机热点一致)"));
    }

    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.println(F("[ESP-NOW] 设置信道失败"));
        return false;
    }
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[ESP-NOW] 初始化失败"));
        return false;
    }
    s_espnow_ok = true;

    /* 不用条件判断返回值: 2.x 返回 void, 3.x 返回 esp_err_t */
    esp_now_register_recv_cb(on_espnow_recv);
    esp_now_register_send_cb(on_uplink_sent);

    /* 上行广播 peer 现在就把注册做掉: 注册失败属于"初始化失败", 由 setup 置
     * 降级安全态 (不转发 ENABLE); 之后 uplink_peer_ensure() 还会每 1s 重试,
     * 成功才置 UPLINK_READY (契约 §5.7)。 */
    if (!uplink_peer_ensure(millis())) {
        Serial.println(F("[ESP-NOW] 上行 peer 初始化失败"));
        return false;
    }

    /* 接收方也要把主机注册成 peer, 否则某些 core 版本收不到单播 */
#if ESPNOW_FILTER_MASTER
    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, g_master_mac, 6);
    peer.channel = channel;
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;
    if (!esp_now_is_peer_exist(g_master_mac)) {
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println(F("[ESP-NOW] 主机 peer 注册失败"));
            return false;    /* 白名单模式下注册失败 = 收不到主机帧, 按初始化失败处理 */
        }
    }
#endif

    Serial.printf("[ESP-NOW] 就绪 信道=%d 本机MAC=%s\n",
                  channel, WiFi.macAddress().c_str());
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

    const uint32_t now = millis();

    out->link_up       = g_link_up;
    out->rssi          = g_last_rssi;
    out->rx_ok         = g_rx_ok;
    out->rx_bad        = g_rx_bad;
    out->rx_dup        = g_rx_dup;
    out->fwd_ok        = g_fwd_ok;
    out->failsafe_sent = g_failsafe_sent;
    out->status_rx     = g_status_rx;
    out->uptime_ms     = now;
    out->motion_blocked = g_motion_blocked;
    out->failsafe      = g_failsafe_active;
    out->degraded      = s_bridge_degraded;
    out->overflows     = g_rx_overflow;

    /* g_last_status / g_stm_fb 是结构体, 可能被桥接任务写到一半。
     * 短临界区整体拷贝一份, 避免显示读到撕裂的浮点数或"新年龄 + 旧状态"。 */
    lc_fresh_t fresh;
    bool have = false;
    portENTER_CRITICAL(&g_status_mux);
    fresh = g_stm_fb;
    have = g_have_status;
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

    bool have_age = false;
    const uint32_t age = lc_fresh_age(&fresh, now, &have_age);
    out->stm32_age_ms = have_age ? age : 0xFFFFFFFFu;
    out->stm32_fresh  = have_age && (age <= BRIDGE_STM_FRESH_MS);
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

    /* linkcore 上下文 (纯 C, 无 RTOS 依赖) */
    lc_seq_gate_init(&g_seq);
    lc_rx_init(&g_stm_rx);
    lc_fresh_init(&g_stm_fb);
    lc_txstats_init(&g_up_tx);

    bool ok = true;

    s_safety_q = xQueueCreate(BRIDGE_SAFETY_QUEUE_DEPTH, sizeof(rx_item_t));
    s_cmd_q    = xQueueCreate(BRIDGE_CMD_QUEUE_DEPTH, sizeof(rx_item_t));
    if (s_safety_q == nullptr || s_cmd_q == nullptr) {
        ok = false;
        Serial.println(F("[致命] 接收队列创建失败 -> 进入降级安全态"));
    }

    if (!espnow_start()) {
        ok = false;
        Serial.println(F("[致命] ESP-NOW 起不来 -> 进入降级安全态"));
    }

    /* 从这里开始算"主机静默" (必须在 espnow_start 之后、任务启动之前设置):
     * WiFi 连接最长要等 12s, 若从更早的时刻算起, 任务一启动就会误判"主机早就
     * 静默"并进入 FAILSAFE 清一次队列。 */
    g_last_host_ms = millis();
    g_last_log_ms  = millis();

    /* 桥接任务放在核心 1, 与 WiFi 任务分开, 减少抖动 */
    const BaseType_t rc = xTaskCreatePinnedToCore(bridge_task, "bridge", 4096,
                                                  nullptr, 5, &g_bridge_task, 1);
    if (rc != pdPASS) {
        g_bridge_task = nullptr;
        ok = false;
        Serial.println(F("[致命] 桥接任务创建失败 -> 进入降级安全态 (由主循环兜底注入失能帧)"));
    }

    s_bridge_degraded = !ok;
    if (s_bridge_degraded) {
        g_motion_blocked = true;
        Serial.println(F("[安全] 降级安全态: 不转发任何 ENABLE, 20Hz 持续注入失能帧; 需复位修复"));
    }

    /* ★ 显示任务放在核心 0:
     *   软件 I2C 刷一屏要几毫秒, 放在网桥任务里会影响 ESP-NOW 的实时性。
     *   单独一个低优先级任务, 刷新慢一点也无所谓 (10Hz 足够看)。
     *   显示任务创建失败只影响屏幕, 与安全逻辑无关, 不置降级态。 */
    if (xTaskCreatePinnedToCore(display_task, "oled", 3072, nullptr, 2, nullptr, 0) != pdPASS) {
        Serial.println(F("[OLED] 显示任务创建失败 (不影响桥接与安全功能)"));
    }
}

/* ================================================================== */
/* 串口配置台 (只做配网, 平时不用)                                     */
/* ================================================================== */

static void console_poll(void)
{
    static char line[96];
    static uint8_t len = 0;

    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\r') {
            continue;
        }
        if (c != '\n') {
            if (len < sizeof(line) - 1) {
                line[len++] = c;
            } else {
                len = 0;
            }
            continue;
        }
        line[len] = '\0';
        len = 0;
        if (line[0] == '\0') {
            continue;
        }

        if (line[0] == 'J') {
            char *comma = strchr(line + 1, ',');
            if (comma == NULL || comma == line + 1) {
                Serial.println(F(">> 用法: J <WiFi名>,<密码>"));
                continue;
            }
            *comma = '\0';
            const char *ssid = line + 1;
            const char *pass = comma + 1;
            if (strlen(pass) < 8) {
                Serial.println(F(">> 密码至少 8 位"));
                continue;
            }
            if (netcfg_save(ssid, pass)) {
                Serial.printf(">> WiFi 已保存: \"%s\", 3 秒后重启\n", ssid);
                Serial.flush();
                delay(3000);
                ESP.restart();
            } else {
                Serial.println(F(">> 保存失败"));
            }
        } else if (line[0] == 'D') {
            netcfg_clear();
            Serial.println(F(">> 已清除 WiFi 账号, 3 秒后重启 (固定信道 6)"));
            Serial.flush();
            delay(3000);
            ESP.restart();
        } else if (line[0] == 'b') {
            Serial.println(F(">> 重启..."));
            Serial.flush();
            delay(300);
            ESP.restart();
        } else if (line[0] == 'm' || line[0] == 'h' || line[0] == '?') {
            Serial.println(F("桥接配置台:"));
            Serial.println(F("  J <WiFi名>,<密码>   配家里 WiFi (与主机用同一个)"));
            Serial.println(F("  D                   清除 WiFi 账号"));
            Serial.println(F("  b                   重启"));
        } else {
            Serial.printf("?? 未知命令 '%c', 发 m 看帮助\n", line[0]);
        }
    }
}

void loop(void)
{
    /* 全部工作都在 bridge_task / display_task 里, 这里只做串口配网 + 兜底心跳 */
    console_poll();

    /* 桥接任务没创建成功时, 主循环兜底注入失能帧:
     * 降级安全态也必须有 20Hz 的失能帧, 不能让 STM32 只能靠自己的 300ms 超时。 */
    if (s_bridge_degraded && g_bridge_task == nullptr) {
        static uint32_t s_loop_inject_ms = 0;
        const uint32_t now = millis();
        if ((uint32_t)(now - s_loop_inject_ms) >= BRIDGE_FAILSAFE_PERIOD_MS) {
            s_loop_inject_ms = now;
            inject_failsafe_frame();
        }
    }

    delay(10);
}
