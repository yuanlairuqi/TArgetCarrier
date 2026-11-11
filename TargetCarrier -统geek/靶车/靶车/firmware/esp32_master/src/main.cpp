/**
 * @file    main.cpp
 * @brief   靶车 —— ESP32 主机 (发射端)
 *
 *   职责:
 *     - 自主产生云台运动指令 (正弦 / 三角扫描 / 圆 / 8 字 / 定点 / 手动)
 *     - 以 CTRL_LOOP_HZ(默认 50Hz) 通过 ESP-NOW 广播 17 字节控制帧
 *     - USB 串口接受在线命令, 实时切换轨迹与调参
 *
 *   本固件可烧到任意 ESP32 开发板 (ESP32 / ESP32-S3 均可)。
 *
 *   ★ v1.1 通信可靠性改造 (依据 verification/freertos_port_20261024/DESIGN_CONTRACT.md §4):
 *     1) 安全命令旁路: x/C/R 由 WS 任务/HTTP/串口直接写入原子旁路槽 s_safety_cmd,
 *        不经 WS 命令队列, 队列满也不丢; loop 每轮最先执行, 执行后清空排队中的
 *        旧普通命令, 防止"停车后继续执行旧的启动命令"。
 *     2) 运动授权: 只有用户明确按 E 才调用 lc_arm_tx_next() 生成新令牌, 之后持续
 *        发 ENABLE|ARM|token; C/x/安全事件清 ENABLE+ARM+token 并 lc_arm_tx_clear;
 *        R 只解除急停 —— 绝不恢复使能, 恢复运动必须重新按 E。
 *     3) SAFE 状态机: 桥接新鲜度帧/STM32 状态帧超过 300ms 不新鲜、桥接报
 *        FAILSAFE/MOTION_BLOCKED、ESP-NOW 发送在途超时 → 进入 SAFE
 *        (撤销授权、清队列、停巡航/速度模式); 链路恢复后仍需用户重新按 E。
 *     4) 发送统计区分 提交/回调/应用确认 (lc_txstats), 不再把"提交成功"当成
 *        "已送达"; 回调在途超时 250ms。
 *
 *   串口命令 (115200 或 app_config.h 里 SERIAL_BAUD):
 *     h          帮助
 *     0..5       切换轨迹: 0 定点 1 正弦 2 三角扫描 3 圆周 4 八字 5 手动
 *     a <度>     正弦幅度
 *     t <秒>     正弦周期
 *     A <度>     扫描幅度
 *     v <度/秒>  扫描角速度
 *     r <度>     圆周半径
 *     y <度>     手动偏航角 (自动切入手动模式)
 *     p <度>     手动俯仰角
 *     k <kp>     MIT 位置增益 (0~500)
 *     d <kd>     MIT 阻尼增益 (0~5)
 *     e          发一次"清错"标志
 *     z          发一次"保存零点"标志 (务必确认安全后再用)
 *     E          使能电机 (生成新授权令牌; 要求桥接与 STM32 都新鲜、急停已解除)
 *     C          失能电机 (安全旁路: 清 ENABLE/ARM/令牌, 清待执行命令)
 *     x          急停 (安全旁路: 锁存 ESTOP, 立即撤销使能与授权)
 *     R          解除急停 (安全旁路: 只清 ESTOP, 不会自动恢复使能)
 *     s          立即打印一次状态
 *
 *   保护时限 (三端统一, 契约 §2): 控制命令最大年龄 100ms、链路/反馈新鲜度 300ms、
 *   ESP-NOW 发送回调在途超时 250ms。
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

#include "protocol.h"
#include "linkcore.h"     /* v1.1: 授权令牌/新鲜度/发送统计 (三端共用的纯 C 内核) */
#include "app_config.h"
#include "motion_profile.h"
#include "netcfg.h"
#include "param_store.h"
#include "web_ui.h"
#include "ws_server.h"
#include "display_lcd.h"

/* 允许用 ESP_ARDUINO_VERSION_MAJOR 做条件编译; 老核心不带这个宏 */
#ifndef ESP_ARDUINO_VERSION_MAJOR
#define ESP_ARDUINO_VERSION_MAJOR 2
#endif

/* ================================================================== */
/* 全局状态                                                           */
/* ================================================================== */

static motion_profile_t  g_profile;
static ctrl_target_t     g_target;
static uint8_t           g_seq          = 0;
/* ★ 上电默认不带 ENABLE: 云台保持失能(自由/无阻尼), 不会自己动。
 *   要动手就在控制台发命令 (例如先 k 30、d 1, 再 0..5 选轨迹) ——
 *   使能位由第一次有效命令打开, 见 handle_command()。
 *   原来这里是 CTRL_FLAG_ENABLE, 一上电云台就闭环顶着, 第一次接负载太危险。 */
static uint16_t          g_flags        = 0;
static uint8_t           g_motor_mode   = MOTOR_MODE_MIT;
/* 速度模式: 用户直接给角速度 (dps), 主机把它积分成目标角, 同时下发速度前馈。
 * 这对应达妙上位机里"速度"那一页的用法, 适合连续转/慢速扫。 */
static float             g_vel_yaw_dps   = 0.0f;
static float             g_vel_pitch_dps = 0.0f;
static bool              g_vel_mode      = false;

/* 巡航模式: 以固定速度在 ±g_cruise_amp 之间自动往返 */
static bool              g_cruise_mode   = false;
static float             g_cruise_dps    = 20.0f;
static float             g_cruise_amp    = 60.0f;
static int8_t            g_cruise_dir    = 1;

/* 单播失败监控: 连续发不出去且收不到桥接回传时, 自动退回广播 */
static bool              g_use_bcast    = (ESPNOW_USE_BROADCAST != 0);

static float             g_kp           = GIMBAL_KP_DEFAULT;
static float             g_kd           = GIMBAL_KD_DEFAULT;

/* ★ v1.1 运动授权/安全状态 (契约 §4.2 / §4.3):
 *   g_flags 里的 ENABLE/ARM/令牌 只允许 "E" 打开; 其它任何路径 (C / x / SAFE)
 *   都必须走 auth_drop() 统一清除, 避免漏清一位导致"断联恢复后自动恢复运动"。 */
static lc_arm_tx_t       g_arm;                /* 授权令牌发生器: 只在 E 时 next() */
static bool              g_estop        = false;
static bool              g_safe         = false;  /* 安全状态机 (0/1) */
static uint8_t           g_safe_reason  = 0;      /* 1..5, 与 web/index.html 的文案一一对应 */

/* 安全命令旁路槽 (契约 §4.1): WS 任务 / HTTP / 串口 识别到 x/C/R 时直接写这里,
 * 不经过命令队列; loop 每轮最先消费。Xtensa 上 1/4 字节对齐的读写是原子的,
 * 跨核写不需要额外加锁, 只需保证"先写命令码, 再自增序号"的顺序。 */
static uint32_t s_safety_pending = 0; /* Atomic bitset: independent stops cannot overwrite each other. */
static uint32_t s_safety_epoch = 0;
static bool s_reboot_pending = false;
static uint32_t s_reboot_at_ms = 0;
static uint8_t s_requested_token = 0; /* Published to RX callback by the loop owner. */
static uint8_t s_confirmed_token = 0;
static uint32_t s_enable_request_ms = 0;


static uint32_t          g_last_loop_ms = 0;
static uint32_t          g_last_log_ms  = 0;
static char              g_line[64];
static uint8_t           g_line_len     = 0;

/* 增益下发 (PROTO_MSG_GAINS): 改过就立刻发, 否则每秒补发一次抗丢包 */
static bool              s_gains_dirty   = true;
static uint32_t          s_gains_last_ms = 0;
static uint8_t           s_gains_seq     = 0;

/* 板载 ST7789 屏刷新节拍 */
#define LCD_UPDATE_MS    200u
static uint32_t          s_lcd_last_ms   = 0;

/* ★ v1.1 ESP-NOW 发送统计 (契约 §4.4): 提交 / 异步回调 / 应用确认 三者分开记账。
 *   发送回调在 WiFi 任务(核心0)里执行, loop 在核心1, 所以对 s_tx 的访问统一用
 *   临界区串起来 (portMUX 是 esp32 Arduino 框架自带 API, 不引入新依赖)。 */
static lc_txstats_t      s_tx;
static portMUX_TYPE      s_tx_mux        = portMUX_INITIALIZER_UNLOCKED;
static uint32_t          s_cb_timeout_streak = 0;   /* 连续在途超时计数 -> SAFE 理由 */

/* ★ v1.1 链路新鲜度 (契约 §4.5): 只有真正收到对应帧才 kick, 旧缓存值绝不刷新在线时间 */
static lc_fresh_t        g_stm_fresh;       /* STM32 STATUS 帧新鲜度 */
static lc_fresh_t        g_bridge_fresh;    /* 桥接 PROTO_MSG_BRIDGE_STATUS 帧新鲜度 */
static volatile uint8_t  s_br_flags          = 0;   /* 最近一帧桥接诊断 flags */
static volatile uint32_t s_br_stm_age_ms     = 0;   /* 桥接报告的 STM32 反馈年龄 (诊断) */
static volatile uint8_t  s_br_arm_rej_l8     = 0;   /* 桥接 arm_reject 低 8 位 (做增量累计) */
static volatile uint32_t s_br_arm_rej_total  = 0;   /* 累计授权拒绝次数 (JSON arm_rej) */

/* WS 命令队列: 普通命令走队列, 安全命令走 s_safety_cmd 旁路 (契约 §4.1) */
#define WS_CMD_Q_LEN      8
#define WS_CMD_Q_ITEM     64
/* 队列元素 64 字节 = 命令文本(最多 54) + 入队时间戳 + 停止代次。
 * 时间戳用于出队时做过期判定 (契约 §2 的 100ms), 旧命令绝不能再当成新命令执行。 */
#define WS_CMD_TEXT_MAX   54u
#define WS_CMD_TS_OFF     56u
#define WS_CMD_EPOCH_OFF  60u
static QueueHandle_t     s_ws_cmd_q      = nullptr;
static volatile uint32_t g_ws_cmd_drop   = 0;   /* 队列满丢弃的普通命令数 (JSON q_drop) */
static volatile uint32_t s_cmd_age_drop  = 0;   /* 出队时已过期丢弃数 (JSON cmd_age_drop) */
static volatile uint32_t s_ws_cmd_purged = 0;   /* 安全事件清队列作废的旧命令数 (只打印) */

/* NVS 参数保存失败次数 (契约 §4.9: 保存失败要计数并打印, 不能静默丢参数) */
static uint32_t          s_param_save_fail = 0;

/* 桥接回传的 STM32/电机状态 (只做显示; "是否在线/新鲜"一律看 g_stm_fresh,
 *  不再用"缓存最后一帧的时间"冒充在线, 见契约 §4.5) */
static volatile float    s_stm_yaw    = 0.0f;
static volatile uint8_t  s_stm_temp   = 0;
static volatile uint8_t  s_stm_err    = 0;
static volatile uint8_t  s_stm_state  = 0;
static volatile uint32_t s_stm_frames = 0;
/* 电机实时量 (来自 STM32, 20 Hz) */
static volatile float    s_motor_pos = 0.0f;
static volatile float    s_motor_vel = 0.0f;
static volatile float    s_motor_tau = 0.0f;
static volatile uint16_t s_motor_cf  = 0xFFFFu;
/* v1.1 电机帧扩展安全位 (p[12..14]): 用来显示 STM32 侧"实际已授权/令牌回显" */
static volatile uint8_t  s_motor_safety   = 0;
static volatile uint8_t  s_motor_arm_echo = 0;
static volatile uint32_t s_motor_rx_ms = 0;
static volatile bool s_have_motor = false;

/* 电机参数缓存 (最多 64 个 RID, 由 /api/param 读写) */
#define PARAM_SLOTS  64
static uint8_t  s_param_have[PARAM_SLOTS];
static uint8_t  s_param_val[PARAM_SLOTS][4];
static uint8_t  s_param_seq = 0;

/* 参数持久化 (NVS): 改过可保存参数后, 1.5 s 没有新改动就写 Flash */
static bool              s_param_dirty    = false;
static uint32_t          s_param_dirty_ms = 0;

/* 定义在 handle_command 之后, 这里先声明 (handle_command 要用) */
static void params_mark_dirty(void);
static void params_save_now(void);

/* ================================================================== */
/* 工具                                                               */
/* ================================================================== */

/* s_tx 被 loop (提交/超时/确认) 与 WiFi 任务 (发送回调) 共享, 所有访问都套这把锁。
 * 临界区只包住几十条指令的记账操作, 不会影响 50Hz 发帧节拍。 */
static inline void tx_lock(void)
{
    portENTER_CRITICAL(&s_tx_mux);
}

static inline void tx_unlock(void)
{
    portEXIT_CRITICAL(&s_tx_mux);
}

static void print_help(void)
{
    Serial.println();
    Serial.println(F("========= 靶车 ESP32 主机控制台 ========="));
    Serial.println(F("轨迹: 0=定点 1=正弦 2=三角扫描 3=圆周 4=八字 5=手动"));
    Serial.println(F("  0..5        切换轨迹 (以当前角度为起点, 不会突跳)"));
    Serial.println(F("  a <度>      正弦幅度"));
    Serial.println(F("  t <秒>      正弦周期"));
    Serial.println(F("  A <度>      扫描幅度"));
    Serial.println(F("  v <度/秒>   扫描角速度"));
    Serial.println(F("  r <度>      圆周半径"));
    Serial.println(F("  y <度>      手动偏航角 (自动进手动模式)"));
    Serial.println(F("  p <度>      手动俯仰角"));
    Serial.println(F("  k <kp>      MIT 位置增益 0~500"));
    Serial.println(F("  d <kd>      MIT 阻尼增益 0~5"));
    Serial.println(F("  e           清电机错误      z 保存零点"));
    Serial.println(F("  w           立即存参数到 Flash (改增益/幅度 1.5s 后也会自动存)"));
    Serial.println(F("  b           保存参数并软重启 (只能从串口发, 网页 /api/cmd 拒绝)"));
    Serial.println(F("  J <名>,<密码>  配家里 WiFi (存 NVS 后自动重启; 只能从串口发)"));
    Serial.println(F("  D           清除家里 WiFi 账号 (只留热点 TargetCar; 只能从串口发)"));
    Serial.println(F("  E           使能电机 (需急停已解除 + 桥接/STM32 都新鲜; 生成新授权令牌)"));
    Serial.println(F("  C           失能电机 (安全旁路: 清 ENABLE/ARM/令牌, 清待执行命令)"));
    Serial.println(F("  x           急停 (安全旁路: 锁存 ESTOP, 撤销使能与授权)"));
    Serial.println(F("  R           解除急停 (只清 ESTOP, 不会自动恢复使能; 要动请重新按 E)"));
    Serial.println(F("  s           打印一次状态    m 本帮助"));
    Serial.println(F("保护时限: 控制命令 100ms 过期 / 链路与反馈 300ms 新鲜度 / ESP-NOW 回调 250ms"));
    Serial.println(F("========================================="));
}

static void print_status(void)
{
    /* 统计口径 (契约 §4.4): 提交(sub) 只代表esp_now_send入队, 回调(cb)才是链路确认。
     * 这里显示回调确认计数, 不再把提交成功打印成"TX ok"。 */
    uint32_t cb_ok, cb_fail, sub_ok, sub_fail, cb_to;
    tx_lock();
    cb_ok    = s_tx.cb_ok[LC_TX_CTRL];
    cb_fail  = s_tx.cb_fail[LC_TX_CTRL];
    sub_ok   = s_tx.submit_ok[LC_TX_CTRL];
    sub_fail = s_tx.submit_fail[LC_TX_CTRL];
    cb_to    = s_tx.cb_timeout;
    tx_unlock();

    const char *state;
    if (g_estop) {
        state = "【急停】";
    } else if (g_safe) {
        state = "【SAFE】";
    } else if (g_flags & CTRL_FLAG_ENABLE) {
        state = "【使能】";
    } else {
        state = "【失能】";
    }

    Serial.printf("[%8lu] 轨迹=%-7s yaw=%7.2f pitch=%7.2f  "
                  "rate=(%6.1f,%6.1f) kp=%.1f kd=%.2f  "
                  "TX cb=%lu/%lu sub=%lu/%lu to=%lu tok=%u %s\n",
                  (unsigned long)millis(),
                  motion_profile_name(g_profile.id),
                  g_target.yaw_deg, g_target.pitch_deg,
                  g_target.yaw_rate_dps, g_target.pitch_rate_dps,
                  g_kp, g_kd,
                  (unsigned long)cb_ok, (unsigned long)cb_fail,
                  (unsigned long)sub_ok, (unsigned long)sub_fail,
                  (unsigned long)cb_to,
                  (unsigned)proto_flags_token(g_flags), state);
}

/* ================================================================== */
/* ESP-NOW                                                            */
/* ================================================================== */

static uint8_t g_peer_mac[6] = {
    ESPNOW_PEER_MAC_0, ESPNOW_PEER_MAC_1, ESPNOW_PEER_MAC_2,
    ESPNOW_PEER_MAC_3, ESPNOW_PEER_MAC_4, ESPNOW_PEER_MAC_5
};

/* 桥接 → 主机 的上行帧 (17 字节同壳):
 *   PROTO_MSG_BRIDGE_STATUS (0x07) 桥接链路诊断/新鲜度帧
 *   PROTO_MSG_STATUS        (0x02) STM32 状态帧 (桥接只转发新鲜的那部分)
 *   PROTO_MSG_MOTOR         (0x06) 电机实时量 + v1.1 扩展安全位
 *   PROTO_MSG_PARAM_REPLY   (0x05) 电机参数回包
 * ★ 本函数在 WiFi 任务上下文执行, 只做解码+拷贝, 不打印、不阻塞、不发包。 */
static void on_recv(const uint8_t *mac, const uint8_t *data, int len)
{
    (void)mac;
    if (len < (int)STATUS_FRAME_SIZE) {
        return;
    }
    uint32_t now = millis();

    /* ---- v1.1 桥接新鲜度帧: 桥接活着/是否 failsafe/STM32 反馈年龄 ----
     * 以前主机把"收到缓存 STATUS"当成 STM32 在线; 现在桥接不新鲜就不转发,
     * 这一帧才是判断桥接与 STM32 链路状态的唯一依据 (契约 §4.5)。 */
    if (data[2] == (uint8_t)PROTO_MSG_BRIDGE_STATUS) {
        bridge_status_frame_t bs;
        if (proto_decode_bridge_status(data, (uint16_t)len, &bs)) {
            lc_fresh_kick(&g_bridge_fresh, now);
            s_br_flags      = bs.flags;
            s_br_stm_age_ms = (uint32_t)bs.stm_age_10ms * 10u;
            /* arm_reject 在帧里只有低 8 位: 按 8 位增量累计成"可读的累计值"。
             * 帧是模 256 计数的, (uint8_t)(new-old) 天然处理回绕。 */
            uint8_t rej_delta = (uint8_t)(bs.arm_reject_l8 - s_br_arm_rej_l8);
            s_br_arm_rej_l8    = bs.arm_reject_l8;
            s_br_arm_rej_total = s_br_arm_rej_total + rej_delta;

        }
        return;
    }

    /* ---- 电机实时量 (v1.1: p[12..14] 是安全位/丢弃计数/ARM 令牌回显) ---- */
    if (data[2] == (uint8_t)PROTO_MSG_MOTOR) {
        float p_, v_, t_;
        uint16_t cf = 0xFFFFu;
        uint8_t safety = 0, arm_echo = 0;
        if (proto_decode_motor_ext2(data, (uint16_t)len, &p_, &v_, &t_, &cf,
                                    &safety, nullptr, &arm_echo)) {
            s_motor_pos      = p_;
            s_motor_vel      = v_;
            s_motor_tau      = t_;
            s_motor_cf       = cf;
            s_motor_safety   = safety;
            /* STM32 当前已接受的令牌: 主机的"已授权"显示以它为准,
             * 而不是只看本地发过什么 —— 避免把"发了"显示成"已授权"。 */
            s_motor_arm_echo = arm_echo;
            s_motor_rx_ms = now;
            s_have_motor = true;
            uint8_t requested = __atomic_load_n(&s_requested_token, __ATOMIC_ACQUIRE);
            if (requested != 0u && requested == arm_echo && s_confirmed_token != requested &&
                (safety & PROTO_MOTOR_SAFETY_ARMED)) {
                tx_lock();
                lc_txstats_confirm(&s_tx, LC_TX_CTRL);
                s_confirmed_token = requested;
                tx_unlock();
            }
        }
        return;
    }

    /* 参数回包: [0]=HEAD [1]=len [2]=0x05 [3]=seq [4]=rid [5]=ok [6..9]=value */
    if (data[2] == (uint8_t)PROTO_MSG_PARAM_REPLY) {
        uint8_t rid = data[4];
        if (rid < PARAM_SLOTS) {
            s_param_val[rid][0] = data[6];
            s_param_val[rid][1] = data[7];
            s_param_val[rid][2] = data[8];
            s_param_val[rid][3] = data[9];
            s_param_have[rid] = 1;
        }
        return;
    }

    status_frame_t st;
    if (!proto_decode_status(data, (uint16_t)len, &st)) {
        return;
    }
    s_stm_yaw    = st.yaw_deg;
    s_stm_temp   = st.temp_mos;
    s_stm_err    = st.err_code;
    s_stm_state  = st.state;
    s_stm_frames = s_stm_frames + 1u;
    /* ★ 只有真正收到 STATUS 帧才刷新 STM32 新鲜度; 缓存的最后一帧时间不得冒充"现在在线" */
    lc_fresh_kick(&g_stm_fresh, now);
}

static bool radio_send(lc_tx_kind_t kind, uint16_t tag, const uint8_t *data, size_t len)
{
    tx_lock();
    if (lc_txstats_inflight(&s_tx) != 0u) { tx_unlock(); return false; }
    (void)lc_txstats_submit(&s_tx, kind, tag, millis(), true);
    tx_unlock();
    bool ok = esp_now_send(g_peer_mac, data, len) == ESP_OK;
    if (!ok) {
        tx_lock();
        s_tx.count = 0; /* Rejected API submissions have no callback. Only one submission is allowed. */
        s_tx.submit_ok[kind]--;
        s_tx.submit_fail[kind]++;
        tx_unlock();
    }
    return ok;
}

/* 主机 -> 桥接 -> STM32 -> CAN 0x7FF: 读/写/保存电机参数 */
static bool param_request(uint8_t op, uint8_t rid, const uint8_t value[4])
{
    uint8_t frame[CTRL_FRAME_SIZE];
    uint8_t seq = s_param_seq++;
    proto_encode_param(frame, op, rid, value, seq);
    return radio_send(LC_TX_PARAM, seq, frame, sizeof(frame));
}

static void on_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    /* ★ 发送回调在 WiFi 任务里执行; lc_txstats_cb 按提交顺序出队记账,
     *   与 loop 的提交/超时共享 s_tx, 所以套临界区。 */
    uint32_t now = millis();
    tx_lock();
    lc_txstats_cb(&s_tx, status == ESP_NOW_SEND_SUCCESS, now);
    tx_unlock();
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
/* core 3.x 的发送回调签名不一样 (第一个参数是 wifi_tx_info_t*) */
static void on_sent_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    on_sent(nullptr, status);
}
#endif

/* ================================================================== */
/* WiFi: 优先连家里路由器 (同一网下直接用浏览器开页面); 失败退热点配网 */
/* ================================================================== */

static char s_wifi_ssid[33];
static char s_wifi_pass[65];
static bool     s_sta_have_creds = false;
static bool     s_sta_connected  = false;
static uint32_t s_sta_last_try   = 0;
static uint8_t  s_espnow_channel = ESPNOW_CHANNEL;

/**
 * ★ 开机流程刻意改成"热点先起、连路由器放后台":
 *   以前是先阻塞等 STA 最多 12 s, 等不到才开热点 —— 手机在这个窗口里连上去
 *   拿不到 IP, 浏览器就一直转圈, 表现就是"打开网址卡很久"。
 *   现在 AP 立刻起来, STA 只是后台重试; 连上之后热点会跟随路由器信道。
 */
static uint8_t wifi_start(void)
{
    WiFi.mode(WIFI_AP_STA);
    WiFi.disconnect();

    /* 1. 热点马上开 (先让手机/电脑能连上来) */
    WiFi.softAP(WEB_AP_SSID, WEB_AP_PASS, ESPNOW_CHANNEL, 0, 4);
    Serial.printf("[WEB] 热点 \"%s\" 密码 \"%s\" 信道=%d (已就绪)\n",
                  WEB_AP_SSID, WEB_AP_PASS, ESPNOW_CHANNEL);
    Serial.printf("[WEB] 现在就能打开 http://%s/ (无需等待)\n",
                  WiFi.softAPIP().toString().c_str());

    /* 2. 有账号就后台开始连, 不在这里等 */
    s_sta_have_creds = netcfg_load(s_wifi_ssid, sizeof(s_wifi_ssid),
                                   s_wifi_pass, sizeof(s_wifi_pass));
    if (s_sta_have_creds) {
        Serial.printf("[WiFi] 后台连接 \"%s\" (热点同时可用)\n", s_wifi_ssid);
        WiFi.begin(s_wifi_ssid, s_wifi_pass);
        s_sta_last_try = millis();
    } else {
        Serial.println(F("[WiFi] 还没配家里 WiFi, 用热点就行; 以后可在 /wifi 页面配"));
    }

    s_espnow_channel = ESPNOW_CHANNEL;
    return s_espnow_channel;
}

/**
 * 每圈 loop 调一次: 处理 STA 重连 + 信道跟随 (都不阻塞)。
 * ESP-NOW 与 WiFi 共用一个射频, STA 一旦连上路由器, AP 和 ESP-NOW 都要跟着换信道。
 */
static void wifi_poll(void)
{
    if (!s_sta_have_creds) {
        return;
    }

    bool now_connected = (WiFi.status() == WL_CONNECTED);

    if (now_connected && !s_sta_connected) {
        s_sta_connected = true;
        uint8_t ch = (uint8_t)WiFi.channel();
        Serial.printf("[WiFi] 已连上 \"%s\", IP=%s, 信道=%d\n",
                      s_wifi_ssid, WiFi.localIP().toString().c_str(), ch);
        Serial.printf("[WEB] 同一网络下打开 http://%s/\n",
                      WiFi.localIP().toString().c_str());

        if (ch != s_espnow_channel) {
            /* 跟着路由器换信道: 先切射频, 再把 peer 的信道改过来 */
            if (esp_wifi_set_channel(ch, WIFI_SECOND_CHAN_NONE) == ESP_OK) {
                esp_now_peer_info_t peer;
                memset(&peer, 0, sizeof(peer));
                memcpy(peer.peer_addr, g_peer_mac, 6);
                peer.channel = ch;
                peer.encrypt = false;
                peer.ifidx   = WIFI_IF_STA;
                esp_now_mod_peer(&peer);      /* 失败也不致命, 下一轮会再试 */
                s_espnow_channel = ch;
                Serial.printf("[ESP-NOW] 信道跟随路由器 -> %d\n", ch);
            }
        }
    } else if (!now_connected && s_sta_connected) {
        s_sta_connected = false;
        Serial.println(F("[WiFi] 与路由器断开, 保持热点, 后台重连"));
        s_sta_last_try = 0;
    }

    if (!now_connected && (uint32_t)(millis() - s_sta_last_try) >= WIFI_STA_RETRY_MS) {
        s_sta_last_try = millis();
        WiFi.begin(s_wifi_ssid, s_wifi_pass);
    }
}

static bool espnow_init(uint8_t channel)
{
    /* 锁死信道: ESP-NOW 与 WiFi 共用射频, 信道漂移会直接断链 */
    if (esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
        Serial.println(F("[ESP-NOW] 设置信道失败!"));
        return false;
    }
    /* 关掉省电, 保证发包时序稳定 */
    esp_wifi_set_ps(WIFI_PS_NONE);

    if (esp_now_init() != ESP_OK) {
        Serial.println(F("[ESP-NOW] 初始化失败!"));
        return false;
    }

#if ESP_ARDUINO_VERSION_MAJOR >= 3
    esp_now_register_send_cb(on_sent_cb);
    esp_now_register_recv_cb(on_recv);
#else
    esp_now_register_send_cb(on_sent);
    /* ★ v2 的回调签名和 v3 不同, 但 on_recv 的参数完全一致, 直接注册即可。
     *   漏了这一行的话: 主机永远收不到桥接回传的 STM32/电机状态,
     *   网页上 st_xxx / m_xxx 全是空, 而编译只会给一条 "on_recv defined but not used"。 */
    esp_now_register_recv_cb(on_recv);
#endif

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, g_peer_mac, 6);
    peer.channel = channel;
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;

    if (!esp_now_is_peer_exist(g_peer_mac)) {
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println(F("[ESP-NOW] 添加对端失败!"));
            return false;
        }
    }

    Serial.printf("[ESP-NOW] 就绪, 信道=%d, 本机MAC=%s\n",
                  channel, WiFi.macAddress().c_str());
    Serial.printf("[ESP-NOW] 对端MAC=%02X:%02X:%02X:%02X:%02X:%02X %s\n",
                  g_peer_mac[0], g_peer_mac[1], g_peer_mac[2],
                  g_peer_mac[3], g_peer_mac[4], g_peer_mac[5],
                  ESPNOW_USE_BROADCAST ? "(广播)" : "(定向)");
    return true;
}

/* ================================================================== */
/* v1.1 安全命令旁路 / 运动授权 / SAFE 状态机 (契约 §4.1 ~ §4.3)        */
/* ================================================================== */

/* 安全命令码 (写进 s_safety_cmd) */
#define SAFETY_NONE   0u
#define SAFETY_DISABLE 1u   /* C: 失能 */
#define SAFETY_ESTOP   2u   /* x: 急停锁存 */
#define SAFETY_RELEASE 3u   /* R: 解除急停 (不恢复使能) */

/* SAFE 原因码 —— 数值与 web/index.html 的 safeReasonText 一一对应:
 *   1=无线/桥接失效 2=STM32 反馈过期 3=ESP-NOW 回调超时 4=桥接 failsafe/拒绝使能 5=其它
 * 用"单一根因"而不是位掩码: 多个条件同时成立时按根因优先级只报一个
 * (桥接都断了就不必再报 STM32 过期, 否则页面/文档的语义会含糊)。 */
#define SAFE_REASON_NONE          0u
#define SAFE_REASON_BRIDGE_STALE  1u
#define SAFE_REASON_STM_STALE     2u
#define SAFE_REASON_TX_TIMEOUT    3u
#define SAFE_REASON_BRIDGE_BLOCK  4u

/* 撤销"可运动授权": 清 ENABLE + ARM + 令牌, 并让本地令牌立即失效。
 * ★ 所有停止/急停/安全路径都必须经过这里, 漏清任一位都可能让电机被旧帧重新使能。 */
static void auth_drop(void)
{
    g_flags = (uint16_t)(g_flags &
              (uint16_t)~(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM | CTRL_FLAG_ARM_TOKEN_MASK));
    lc_arm_tx_clear(&g_arm);
    s_confirmed_token = 0;
    __atomic_store_n(&s_requested_token, 0u, __ATOMIC_RELEASE);
}

/* 停止巡航/速度模式 (只清"自动增量运动", 保留当前轨迹模式选择) */
static void motion_modes_stop(void)
{
    g_cruise_mode   = false;
    g_vel_mode      = false;
    g_vel_yaw_dps   = 0.0f;
    g_vel_pitch_dps = 0.0f;
}

/* 清空 WS 普通命令队列, 返回作废条数。
 * ★ 安全命令执行前必须清: 否则"按下急停/失能之后, 队列里之前排的启动命令还会执行",
 *   表现就是"明明停了又自己动起来"。 */
static uint32_t clear_ws_cmd_queue(void)
{
    if (s_ws_cmd_q == nullptr) {
        return 0;
    }
    char item[WS_CMD_Q_ITEM];
    uint32_t n = 0;
    while (xQueueReceive(s_ws_cmd_q, item, 0) == pdTRUE) {
        n++;
    }
    (void)xQueueReset(s_ws_cmd_q);   /* 双保险: 计数期间新入队的也一并清掉 */
    s_ws_cmd_purged = s_ws_cmd_purged + n;
    return n;
}

/* 识别一行命令是否是安全命令; 返回 0 表示不是。
 * 必须严格匹配 "单个字母 x / C / R", 避免把 "R1" 之类误判成安全命令。 */
static uint8_t safety_cmd_of(const char *line)
{
    return (uint8_t)lc_stop_command(line);
}

static void safety_request(uint8_t code)
{
    if (code == 0u) return;
    (void)__atomic_fetch_add(&s_safety_epoch, 1u, __ATOMIC_ACQ_REL);
    (void)__atomic_fetch_or(&s_safety_pending, 1u << code, __ATOMIC_RELEASE);
}

/* 这些命令会改变运动目标: 急停锁存期间一律拒绝 (契约 §4.2) */
static bool is_motion_command(char cmd)
{
    switch (cmd) {
    case '0': case '1': case '2': case '3': case '4': case '5':
    case 'y': case 'p': case 'V': case 'U': case 'W':
        return true;
    default:
        return false;
    }
}

/* 真正执行安全命令 (只在 loop 上下文调用) */
static void execute_safety(uint8_t code)
{
    switch (code) {
    case SAFETY_DISABLE:
        /* C: 失能 —— 只撤销授权, 不动 ESTOP 锁存 */
        auth_drop();
        Serial.println(F(">> 【安全旁路】失能: 已清 ENABLE/ARM/令牌; 待执行的旧命令已作废"));
        break;

    case SAFETY_ESTOP:
        /* x: 急停 —— 锁存 ESTOP, 同时撤销使能/授权; 解除后仍需重新 E */
        g_estop = true;
        g_flags |= CTRL_FLAG_ESTOP;
        auth_drop();
        motion_modes_stop();
        Serial.println(F(">> 【安全旁路】急停: ESTOP 已锁存, 使能与授权已撤销, 巡航/速度模式已停"));
        Serial.println(F("   解除请发 R; 急停锁存期间会拒绝 0..5/y/p/V/U/W 等改变运动的命令"));
        break;

    case LC_STOP_YAW:
        g_vel_yaw_dps = 0.0f;
        g_cruise_mode = false;
        motion_profile_set(&g_profile, PROFILE_MANUAL, g_target.yaw_deg, g_target.pitch_deg);
        break;
    case LC_STOP_PITCH:
        g_vel_pitch_dps = 0.0f;
        motion_profile_set(&g_profile, PROFILE_MANUAL, g_target.yaw_deg, g_target.pitch_deg);
        break;
    case LC_STOP_CRUISE:
        g_cruise_mode = false;
        g_vel_yaw_dps = 0.0f;
        break;
    case SAFETY_RELEASE:
        /* R: 只清 ESTOP; ★ 绝不恢复 ENABLE/ARM/令牌 (契约 §4.2) */
        g_estop = false;
        g_flags &= (uint16_t)~CTRL_FLAG_ESTOP;
        Serial.println(F(">> 【安全旁路】解除急停: 只清 ESTOP; 使能不会自动恢复, 需重新按 E"));
        break;

    default:
        break;
    }
}

static const char *safe_reason_text(uint8_t reason)
{
    switch (reason) {
    case SAFE_REASON_BRIDGE_STALE:
        return "桥接新鲜度帧超过 300ms 未更新";
    case SAFE_REASON_STM_STALE:
        return "STM32 状态帧超过 300ms 未更新";
    case SAFE_REASON_TX_TIMEOUT:
        return "ESP-NOW callback timed out (delivery unknown)";
    case SAFE_REASON_BRIDGE_BLOCK:
        return "桥接报告 FAILSAFE / MOTION_BLOCKED";
    default:
        return "其它安全原因";
    }
}

/* SAFE 状态机 (契约 §4.3): 每轮 loop 调用一次。
 * 触发: 桥接帧过期 / STM32 状态过期 / ESP-NOW 在途连续超时 / 桥接报 failsafe。
 * 动作: 撤销 ENABLE+ARM+令牌, 清命令队列, 停巡航/速度模式 (保留轨迹选择)。
 * 恢复: 条件全部消失才清 g_safe; 但使能绝不自动恢复, 必须用户重新按 E。 */
static void safe_state_eval(uint32_t now)
{
    uint8_t reason = SAFE_REASON_NONE;

    if (!lc_fresh_ok(&g_bridge_fresh, now, MASTER_BRIDGE_STALE_MS)) {
        reason = SAFE_REASON_BRIDGE_STALE;
    } else if (!lc_fresh_ok(&g_stm_fresh, now, MASTER_STM_STALE_MS) ||
               (((s_br_flags & PROTO_BR_FLAG_STM_FRESH) == 0u) &&
                (s_br_stm_age_ms > MASTER_STM_STALE_MS))) {
        /* 两个证据任一成立就判 STM32 反馈过期:
         *   - 主机自己 >300ms 没收到 STATUS 帧;
         *   - 或者桥接的 0x07 帧明确报 STM_FRESH=0 且 STM32 年龄 >300ms
         *     (桥接看到的情况比主机早一步, 用上它能让 SAFE 更及时)。 */
        reason = SAFE_REASON_STM_STALE;
    } else if (s_cb_timeout_streak > 0u) {
        reason = SAFE_REASON_TX_TIMEOUT;
    } else if ((s_br_flags & (PROTO_BR_FLAG_FAILSAFE | PROTO_BR_FLAG_MOTION_BLOCKED)) != 0u) {
        reason = SAFE_REASON_BRIDGE_BLOCK;
    }

    if (reason == SAFE_REASON_NONE && (g_flags & CTRL_FLAG_ENABLE) && s_have_motor &&
        lc_elapsed_ms(now, s_motor_rx_ms) <= MASTER_STM_STALE_MS &&
        ((s_motor_safety & (PROTO_MOTOR_SAFETY_FAULT | PROTO_MOTOR_SAFETY_CAN_RECOVER |
                            PROTO_MOTOR_SAFETY_LINK_LOST)) ||
         (lc_elapsed_ms(now, s_enable_request_ms) > MASTER_STM_STALE_MS &&
          (!(s_motor_safety & PROTO_MOTOR_SAFETY_ARMED) ||
           s_motor_arm_echo != __atomic_load_n(&s_requested_token, __ATOMIC_ACQUIRE))))) {
        reason = SAFE_REASON_BRIDGE_BLOCK;
    }

    if (reason != SAFE_REASON_NONE) {
        g_safe_reason = reason;
        if (!g_safe) {
            g_safe = true;
            auth_drop();
            motion_modes_stop();
            clear_ws_cmd_queue();
            Serial.printf(">> 【SAFE】进入安全态: %s; 已撤销使能与授权, 恢复后需重新按 E\n",
                          safe_reason_text(reason));
        }
        return;
    }

    if (g_safe) {
        g_safe        = false;
        g_safe_reason = SAFE_REASON_NONE;
        Serial.println(F(">> 【SAFE】链路恢复; 使能不会自动恢复, 请重新按 E"));
    }
}

/* loop 每轮最先调用: 消费安全命令旁路槽 (先于串口/HTTP/WS 队列)
 * ★ 安全命令不经过 xQueueSend, 所以 WS 队列满、HTTP 卡顿都不会丢停止命令。 */
static void poll_safety_bypass(void)
{
    uint32_t pending = __atomic_exchange_n(&s_safety_pending, 0u, __ATOMIC_ACQ_REL);
    if (!pending) return;
    (void)clear_ws_cmd_queue();
    if (pending & (1u << LC_STOP_ESTOP)) {
        execute_safety(SAFETY_ESTOP); /* ESTOP dominates a simultaneous release. */
    } else {
        if (pending & (1u << LC_STOP_DISABLE)) execute_safety(SAFETY_DISABLE);
        if (pending & (1u << LC_STOP_RELEASE)) {
            auth_drop(); /* release never restores a previous request */
            execute_safety(SAFETY_RELEASE);
        }
    }
    if (pending & (1u << LC_STOP_YAW)) execute_safety(LC_STOP_YAW);
    if (pending & (1u << LC_STOP_PITCH)) execute_safety(LC_STOP_PITCH);
    if (pending & (1u << LC_STOP_CRUISE)) execute_safety(LC_STOP_CRUISE);
}

static void schedule_reboot(uint32_t delay_ms)
{
    auth_drop();
    motion_modes_stop();
    (void)clear_ws_cmd_queue();
    s_reboot_pending = true;
    s_reboot_at_ms = millis() + delay_ms;
}

/* ================================================================== */
/* 串口命令                                                           */
/* ================================================================== */

static void handle_command(char *line)
{
    /* 去掉首尾空白 */
    while (*line == ' ' || *line == '\t') line++;
    int len = (int)strlen(line);
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    /* ★ v1.1 安全命令旁路 (契约 §4.1): x/C/R 在这里只写原子旁路槽, 不入队。
     *   真正执行在 loop 最开头的 poll_safety_bypass(), 因此:
     *     - 不受 WS 命令队列是否满影响 (队列满也不会丢停止/急停);
     *     - 同一行命令不会被"槽 + 队列"执行两次。 */
    {
        uint8_t sc = safety_cmd_of(line);
        if (sc != SAFETY_NONE) {
            safety_request(sc);
            Serial.printf(">> 安全命令 '%c' 已写入旁路槽, 下一轮 loop 优先执行\n", line[0]);
            return;
        }
    }

    char   cmd = line[0];
    char  *arg = line + 1;
    while (*arg == ' ') arg++;
    float  val = (float)atof(arg);

    /* ★ v1.1 急停锁存期间拒绝一切改变运动目标的命令 (契约 §4.2)。
     *   否则"按了急停后网页滑杆/旧脚本继续改目标", 一解除急停就会突然运动。 */
    if (g_estop && is_motion_command(cmd)) {
        Serial.printf(">> 拒绝命令 '%c': 急停锁存中 (先发 R 解除急停, 再发 E 重新使能)\n", cmd);
        return;
    }

    /* 这几个命令改的是"可保存参数": 处理完标脏, loop 里 1.5 s 后自动落盘 */
    bool param_cmd = (cmd == 'a' || cmd == 't' || cmd == 'A' || cmd == 'v' ||
                      cmd == 'r' || cmd == 'k' || cmd == 'd');

    switch (cmd) {

    /* ---- 轨迹切换 ---- */
    case '0': case '1': case '2': case '3': case '4': case '5':
        g_cruise_mode = false;
        /* 切轨迹必须退出速度模式, 否则速度前馈会一直覆盖轨迹自己的角速度 */
        g_vel_mode      = false;
        g_vel_yaw_dps   = 0.0f;
        g_vel_pitch_dps = 0.0f;
        motion_profile_set(&g_profile, (profile_id_t)(cmd - '0'),
                           g_target.yaw_deg, g_target.pitch_deg);
        Serial.printf(">> 轨迹切换为 %s\n", motion_profile_name(g_profile.id));
        break;

    case 'a':
        g_profile.sine_amp_deg = proto_clampf(val, 0.0f, 180.0f);
        Serial.printf(">> 正弦幅度 = %.1f 度\n", g_profile.sine_amp_deg);
        break;

    case 't':
        g_profile.sine_period_s = proto_clampf(val, 0.2f, 600.0f);
        Serial.printf(">> 正弦周期 = %.2f 秒\n", g_profile.sine_period_s);
        break;

    case 'A':
        g_profile.sweep_amp_deg = proto_clampf(val, 0.0f, 180.0f);
        g_cruise_amp            = g_profile.sweep_amp_deg;   /* 巡航也用这个半幅 */
        Serial.printf(">> 扫描幅度 = %.1f 度\n", g_profile.sweep_amp_deg);
        break;

    case 'v':
        g_profile.sweep_speed_dps = proto_clampf(val, 0.1f, 180.0f);
        Serial.printf(">> 扫描角速度 = %.1f 度/秒\n", g_profile.sweep_speed_dps);
        break;

    case 'r':
        g_profile.circle_radius_deg = proto_clampf(val, 0.0f, 90.0f);
        Serial.printf(">> 圆周半径 = %.1f 度\n", g_profile.circle_radius_deg);
        break;

    /* ---- 手动角度 ---- */
    case 'y':
        /* 手动定位必须退出速度模式/巡航 */
        g_cruise_mode   = false;
        g_vel_mode      = false;
        g_vel_yaw_dps   = 0.0f;
        g_vel_pitch_dps = 0.0f;
        motion_profile_set_manual(&g_profile,
                                  proto_clampf(val, GIMBAL_YAW_MIN_DEG, GIMBAL_YAW_MAX_DEG),
                                  g_profile.manual_pitch_deg);
        if (g_profile.id != PROFILE_MANUAL) {
            motion_profile_set(&g_profile, PROFILE_MANUAL,
                               g_target.yaw_deg, g_target.pitch_deg);
        }
        Serial.printf(">> 手动偏航 = %.2f 度\n", g_profile.manual_yaw_deg);
        break;

    case 'p':
        g_vel_mode      = false;
        g_vel_yaw_dps   = 0.0f;
        g_vel_pitch_dps = 0.0f;
        motion_profile_set_manual(&g_profile,
                                  g_profile.manual_yaw_deg,
                                  proto_clampf(val, GIMBAL_PITCH_MIN_DEG, GIMBAL_PITCH_MAX_DEG));
        if (g_profile.id != PROFILE_MANUAL) {
            motion_profile_set(&g_profile, PROFILE_MANUAL,
                               g_target.yaw_deg, g_target.pitch_deg);
        }
        Serial.printf(">> 手动俯仰 = %.2f 度\n", g_profile.manual_pitch_deg);
        break;

    /* ---- 速度模式 (对应达妙上位机的"速度"页) ---- */
    case 'V': {
        float v = proto_clampf(val, -90.0f, 90.0f);
        g_vel_yaw_dps = v;
        g_vel_mode = true;
        motion_profile_set(&g_profile, PROFILE_MANUAL, g_target.yaw_deg, g_target.pitch_deg);
        if (v == 0.0f) {
            Serial.println(F(">> 速度 0: 停止 (保持当前角度)"));
        } else {
            Serial.printf(">> 速度模式: yaw %.1f 度/秒 (发 V 0 停)\n", v);
        }
        break;
    }

    case 'U': {
        float v = proto_clampf(val, -60.0f, 60.0f);
        g_vel_pitch_dps = v;
        g_vel_mode = true;
        if (v == 0.0f) {
            Serial.println(F(">> pitch 速度 0"));
        } else {
            Serial.printf(">> 速度模式: pitch %.1f 度/秒\n", v);
        }
        break;
    }

    /* ---- 巡航: W <度/秒>, 在 ±A 之间自动往返; W 0 停 ---- */
    case 'W': {
        if (val == 0.0f) {
            g_cruise_mode = false;
            g_vel_yaw_dps = 0.0f;
            Serial.println(F(">> 巡航停止 (保持在当前位置)"));
            break;
        }
        g_cruise_dps  = proto_clampf(val < 0 ? -val : val, 1.0f, 90.0f);
        g_cruise_mode = true;
        g_vel_mode    = false;
        g_vel_yaw_dps = 0.0f;
        g_cruise_dir  = (val < 0.0f) ? -1 : 1;
        motion_profile_set(&g_profile, PROFILE_MANUAL, g_target.yaw_deg, g_target.pitch_deg);
        g_profile.manual_yaw_deg = g_target.yaw_deg;
        Serial.printf(">> 巡航: %.1f 度/秒, 在 ±%.1f 度之间往返 (W 0 停)\n",
                      g_cruise_dps, g_cruise_amp);
        break;
    }

    /* ---- 增益 (带安全上限, 超过会被夹住并提示) ---- */
    case 'k': {
        float v = proto_clampf(val, 0.0f, GIMBAL_KP_SAFE_MAX);
        if (val > GIMBAL_KP_SAFE_MAX) {
            Serial.printf(">> kp 超安全上限, 已夹到 %.0f (要更大请改 app_config.h)\n",
                          GIMBAL_KP_SAFE_MAX);
        }
        g_kp = v;
        s_gains_dirty = true;
        Serial.printf(">> kp = %.1f (实际输出会按斜坡逼近)\n", g_kp);
        break;
    }

    case 'd': {
        float v = proto_clampf(val, 0.0f, GIMBAL_KD_SAFE_MAX);
        if (val > GIMBAL_KD_SAFE_MAX) {
            Serial.printf(">> kd 超安全上限, 已夹到 %.1f\n", GIMBAL_KD_SAFE_MAX);
        }
        if (v < GIMBAL_KD_MIN_ENABLE) {
            Serial.printf(">> 注意: kd=%.2f 太小, 位置控制会震荡, 使能前请 >= %.1f\n",
                          v, GIMBAL_KD_MIN_ENABLE);
        }
        g_kd = v;
        s_gains_dirty = true;
        Serial.printf(">> kd = %.2f\n", g_kd);
        break;
    }

    /* ---- 一次性动作 ---- */
    case 'e':
        g_flags |= CTRL_FLAG_CLEAR_ERR;
        Serial.println(F(">> 发送清错标志 (下一帧生效)"));
        break;

    case 'z':
        g_flags |= CTRL_FLAG_SET_ZERO;
        Serial.println(F(">> 发送保存零点标志! 确认当前位置就是要作为零点的位置!"));
        break;

    /* ---- 安全命令 x / C / R 已在函数开头由旁路槽接管, 不会走到 switch ----
     *   这里刻意不保留 case 'x'/'C'/'R': 防止以后有人再加一条"直接改 g_flags"
     *   的路径绕过原子旁路 (契约 §4.1/§4.2)。 */

    /* ---- 使能 (上电默认失能; ★ 只有这里能生成新的 ARM 令牌) ---- */
    case 'E': {
        if (s_reboot_pending) { Serial.println(F("reboot pending; enable rejected")); break; }
        uint32_t now = millis();

        /* 急停自锁时直接提示, 避免"点了使能没反应"的困惑 */
        if (g_estop) {
            Serial.println(F(">> 拒绝使能: 急停锁存中 (先发 R 解除急停)"));
            break;
        }
        /* SAFE 未恢复时拒绝: 链路不正常时发 ENABLE 只会被桥接/STM32 拒绝 */
        if (g_safe) {
            Serial.printf(">> 拒绝使能: 处于 SAFE 安全态 (原因 %d: %s), 等链路恢复后重试\n",
                          (int)g_safe_reason, safe_reason_text(g_safe_reason));
            break;
        }
        /* ★ 使能前的安全检查: kd 太小会自激, kp 过大容易烧线圈 */
        if (g_kp <= 0.0f) {
            Serial.println(F(">> 拒绝使能: kp=0, 电机不会出力 (先 k 40)"));
            break;
        }
        if (g_kp > GIMBAL_KP_SAFE_MAX) {
            Serial.printf(">> 拒绝使能: kp=%.0f 超过安全上限 %.0f\n",
                          g_kp, GIMBAL_KP_SAFE_MAX);
            break;
        }
        if (g_kd < GIMBAL_KD_MIN_ENABLE) {
            Serial.printf(">> 拒绝使能: kd=%.2f < %.1f, 位置控制会震荡 (先 d 1.5)\n",
                          g_kd, GIMBAL_KD_MIN_ENABLE);
            break;
        }
        /* ★ v1.1 契约 §4.2: 桥接与 STM32 都必须新鲜才允许授权。
         *   链路半断时就算发了令牌, STM32 也可能在主机看不到反馈的情况下使能。 */
        if (!lc_fresh_ok(&g_bridge_fresh, now, MASTER_BRIDGE_STALE_MS)) {
            Serial.printf(">> 拒绝使能: 桥接新鲜度帧超过 %u ms 未更新 (检查主机-桥接 ESP-NOW 链路)\n",
                          (unsigned)MASTER_BRIDGE_STALE_MS);
            break;
        }
        if (!lc_fresh_ok(&g_stm_fresh, now, MASTER_STM_STALE_MS)) {
            Serial.printf(">> 拒绝使能: STM32 状态帧超过 %u ms 未更新 (检查桥接-STM32 串口链路)\n",
                          (unsigned)MASTER_STM_STALE_MS);
            break;
        }

        /* 生成新的非零令牌: 之后每帧持续发送 ENABLE|ARM|token,
         * 直到失能/急停/SAFE 把它们清掉 (契约 §4.2)。 */
        s_enable_request_ms = millis();
        uint8_t tok = lc_arm_tx_next(&g_arm);
        __atomic_store_n(&s_requested_token, tok, __ATOMIC_RELEASE);
        uint16_t keep = (uint16_t)(g_flags & (CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO));
        g_flags = (uint16_t)(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM | keep);
        g_flags = proto_flags_set_token(g_flags, tok);
        Serial.printf(">> 使能电机 (新授权令牌=%u, kp=%.1f kd=%.2f, 增益从 0 斜坡上来)\n",
                      (unsigned)tok, g_kp, g_kd);
        break;
    }

    case 's':
        print_status();
        break;

    case 'm':
    case 'h':
    case '?':
        print_help();
        break;

    /* ---- 配网: J <WiFi名>,<密码> ---- */
    case 'J':
    {
        char *comma = strchr(arg, ',');
        if (comma == NULL || comma == arg) {
            Serial.println(F(">> 用法: J <WiFi名>,<密码>   例: J rm_2.4G,mypass123"));
            break;
        }
        *comma = '\0';
        const char *ssid = arg;
        const char *pass = comma + 1;
        while (*pass == ' ') {
            pass++;
        }
        if (strlen(pass) < 8) {
            Serial.println(F(">> 密码至少 8 位"));
            break;
        }
        if (netcfg_save(ssid, pass)) {
            Serial.printf(">> WiFi 已保存: \"%s\" , 3 秒后重启去连接\n", ssid);
            schedule_reboot(3000u);
        } else {
            Serial.println(F(">> WiFi 保存失败"));
        }
        break;
    }

    case 'D':
        netcfg_clear();
        Serial.println(F(">> 已清除 WiFi 账号, 3 秒后重启 (只开热点)"));
        schedule_reboot(3000u);
        break;

    case 'w':
        params_save_now();
        break;

    case 'b':
        /* 软重启: 用来验证"参数掉电保存"是否真的生效。
         * ★ 只能从串口发: 网页 /api/cmd 会拒绝 J/D/b, 避免 delay()+重启把
         *   50Hz 控制循环阻塞数秒 (桥接 300ms 就判主机失联并注入 FAILSAFE)。 */
        Serial.println(F(">> 保存参数并重启..."));
        params_save_now();
        schedule_reboot(120u);
        break;

    default:
        Serial.printf("?? 未知命令 '%c', 输入 m 看帮助\n", cmd);
        break;
    }

    if (param_cmd) {
        params_mark_dirty();
    }
}

/* ================================================================== */
/* 参数持久化 (NVS / Flash)                                            */
/* ================================================================== */

#define PARAM_SAVE_DELAY_MS 1500u

static void params_fill(param_blob_t *b)
{
    b->magic            = PARAM_STORE_MAGIC;
    b->kp               = g_kp;
    b->kd               = g_kd;
    b->sine_amp_deg     = g_profile.sine_amp_deg;
    b->sine_period_s    = g_profile.sine_period_s;
    b->sweep_amp_deg    = g_profile.sweep_amp_deg;
    b->sweep_speed_dps  = g_profile.sweep_speed_dps;
    b->circle_radius_deg= g_profile.circle_radius_deg;
}

static void params_apply(const param_blob_t *b)
{
    /* 统一按"安全范围"裁剪一遍: 既防 Flash 里的旧版怪值, 也防预设里存过
     * 超限的 kp (历史上允许 500)。上限见 app_config.h 的 GIMBAL_KP_SAFE_MAX。 */
    g_kp                        = proto_clampf(b->kp,                0.0f, GIMBAL_KP_SAFE_MAX);
    g_kd                        = proto_clampf(b->kd,                0.0f, GIMBAL_KD_SAFE_MAX);
    g_profile.sine_amp_deg      = proto_clampf(b->sine_amp_deg,      0.0f, 180.0f);
    g_profile.sine_period_s     = proto_clampf(b->sine_period_s,     0.2f, 600.0f);
    g_profile.sweep_amp_deg     = proto_clampf(b->sweep_amp_deg,     0.0f, 180.0f);
    g_profile.sweep_speed_dps   = proto_clampf(b->sweep_speed_dps,   0.1f, 180.0f);
    g_profile.circle_radius_deg = proto_clampf(b->circle_radius_deg, 0.0f,  90.0f);
    s_gains_dirty = true;   /* 参数变了, 立刻把新增益发给 STM32 */
}

static void params_mark_dirty(void)
{
    s_param_dirty    = true;
    s_param_dirty_ms = millis();
}

static void params_save_now(void)
{
    param_blob_t b;
    params_fill(&b);

    bool ok = param_store_save(&b);
    s_param_dirty = false;
    if (!ok) {
        /* 契约 §4.9: 参数保存失败不能静默 —— 计数并打印累计失败次数 */
        s_param_save_fail++;
        Serial.printf(">> [NVS] 参数保存失败 (累计 %lu 次), 本次改动掉电后会丢\n",
                      (unsigned long)s_param_save_fail);
    }

    /* 同步把峰值回写到串口, 方便确认"存的就是现在用的" */
    Serial.printf(">> %s kp=%.1f kd=%.2f a=%.0f t=%.1f A=%.0f v=%.0f r=%.0f\n",
                  ok ? "参数已保存到 Flash:" : "参数保存失败:",
                  g_kp, g_kd,
                  g_profile.sine_amp_deg, g_profile.sine_period_s,
                  g_profile.sweep_amp_deg, g_profile.sweep_speed_dps,
                  g_profile.circle_radius_deg);
}

static void params_load(void)
{
    param_blob_t b;
    if (!param_store_load(&b)) {
        Serial.println(F("[NVS] 没有已保存的参数, 用默认值"));
        return;
    }
    params_apply(&b);
    Serial.printf("[NVS] 已加载参数: kp=%.1f kd=%.2f a=%.0f t=%.1f A=%.0f v=%.0f r=%.0f\n",
                  g_kp, g_kd,
                  g_profile.sine_amp_deg, g_profile.sine_period_s,
                  g_profile.sweep_amp_deg, g_profile.sweep_speed_dps,
                  g_profile.circle_radius_deg);
}

static void poll_serial(void)
{
    while (Serial.available() > 0) {
        char c = (char)Serial.read();
        if (c == '\n' || c == '\r') {
            if (g_line_len > 0) {
                g_line[g_line_len] = '\0';
                handle_command(g_line);
                g_line_len = 0;
            }
        } else if (g_line_len < sizeof(g_line) - 1) {
            g_line[g_line_len++] = c;
        } else {
            g_line_len = 0;   /* 溢出丢弃 */
        }
    }
}

/* ================================================================== */
/* 给网页控制台用的两个出口 (web_ui.cpp 调用)                          */
/* ================================================================== */

size_t app_status_json(char *buf, size_t n)
{
    uint32_t now = millis();

    /* ---- 新鲜度 (契约 §4.5): 年龄只来自"真正收到帧时 kick 的时间戳",
     *      旧缓存值绝不刷新在线时间; 没收到过就报 0xFFFFFFFF。 ---- */
    bool st_have = false;
    bool br_have = false;
    uint32_t st_age = lc_fresh_age(&g_stm_fresh, now, &st_have);
    uint32_t br_age = lc_fresh_age(&g_bridge_fresh, now, &br_have);
    bool st_fresh   = lc_fresh_ok(&g_stm_fresh, now, MASTER_STM_STALE_MS);
    /* st_online = 反馈新鲜 且 状态机给出有效状态 (INIT/TIMEOUT 不算在线) */
    bool st_online = st_fresh; /* Communication is online even while the motor is idle. */
    (void)st_have;
    (void)br_have;

    /* ---- 发送统计快照: 回调在 WiFi 任务里更新, 加锁取一份一致副本再格式化 ---- */
    uint32_t sub_ok, sub_fail, cb_ok, cb_fail, cb_to, confirm;
    tx_lock();
    sub_ok   = s_tx.submit_ok[LC_TX_CTRL];
    sub_fail = s_tx.submit_fail[LC_TX_CTRL];
    cb_ok    = s_tx.cb_ok[LC_TX_CTRL];
    cb_fail  = s_tx.cb_fail[LC_TX_CTRL];
    confirm  = s_tx.confirm[LC_TX_CTRL];
    cb_to    = s_tx.cb_timeout;
    tx_unlock();

    /* arm / arm_token 用 STM32 电机帧回显 (p[12] ARMED 位 + p[14] 令牌) 再叠加本机
     * "当前是否仍在请求使能" 一起判断, 而不是"主机发过什么"：
     *   - 本机 C/x/SAFE 清掉 ENABLE 后立即显示未授权, 不用等下一帧电机反馈;
     *   - STM32 没确认前 (例如桥接 FAILSAFE 拦下了 ENABLE) 也不会误显示已授权。 */
    bool arm_req = ((g_flags & (CTRL_FLAG_ENABLE | CTRL_FLAG_ARM)) ==
                    (CTRL_FLAG_ENABLE | CTRL_FLAG_ARM)) && lc_arm_tx_active(&g_arm);
    bool motor_fresh = s_have_motor && lc_elapsed_ms(now, s_motor_rx_ms) <= MASTER_STM_STALE_MS;
    bool token_match = s_motor_arm_echo == __atomic_load_n(&s_requested_token, __ATOMIC_ACQUIRE);
    int arm = arm_req && motor_fresh && token_match && (s_motor_safety & PROTO_MOTOR_SAFETY_ARMED);
    int enabled = motor_fresh && !(s_motor_safety & PROTO_MOTOR_SAFETY_FBK_STALE)
        ? ((s_motor_safety & PROTO_MOTOR_SAFETY_ENABLED) ? 1 : 0) : -1;
    int cb_sync;
    tx_lock(); cb_sync = s_tx.cb_desynced ? 0 : 1; tx_unlock();
    int  arm_token = (int)s_motor_arm_echo;

    /* ★ 字段只增不改名 (契约 §7); 总长必须 < 1000 B (ws_server / web_ui 的缓冲 1024)。 */
    int m = snprintf(buf, n,
                     "{\"mode\":\"%s\",\"yaw\":%.2f,\"pitch\":%.2f,"
                     "\"kp\":%.1f,\"kd\":%.2f,\"en\":%d,\"estop\":%d,"
                     "\"ok\":%lu,\"fail\":%lu,\"up\":%lu,\"heap\":%u,"
                     "\"ry\":%.1f,\"rp\":%.1f,\"vel\":%d,\"cruise\":%d,\"cr_dps\":%.1f,\"cr_amp\":%.1f,"
                     "\"st_yaw\":%.2f,\"st_temp\":%u,\"st_err\":%u,"
                     "\"st_state\":%u,\"st_frames\":%lu,\"st_age\":%lu,"
                     "\"st_fresh\":%d,\"st_online\":%d,"
                     "\"m_pos\":%.3f,\"m_vel\":%.2f,\"m_tau\":%.3f,\"m_cf\":%u,"
                     "\"ch\":%d,\"ws\":%d,"
                     "\"br_age\":%lu,\"br_flags\":%u,"
                     "\"safe\":%d,\"safe_reason\":%u,"
                     "\"arm\":%d,\"arm_token\":%d,\"arm_rej\":%lu,"
                     "\"q_drop\":%lu,\"cmd_age_drop\":%lu,"
                     "\"tx_sub_ok\":%lu,\"tx_sub_fail\":%lu,"
                     "\"tx_cb_ok\":%lu,\"tx_cb_fail\":%lu,"
                     "\"tx_confirm\":%lu,\"cb_timeout\":%lu,\"tx_cb_sync\":%d,\"enabled\":%d}",
                     motion_profile_name(g_profile.id),
                     g_target.yaw_deg, g_target.pitch_deg,
                     g_kp, g_kd,
                     (g_flags & CTRL_FLAG_ENABLE) ? 1 : 0,
                     g_estop ? 1 : 0,
                     (unsigned long)cb_ok,          /* ok/fail 旧字段名保留, 含义=回调确认结果 */
                     (unsigned long)cb_fail,
                     (unsigned long)(now / 1000u),
                     (unsigned)ESP.getFreeHeap(),
                     g_target.yaw_rate_dps, g_target.pitch_rate_dps,
                     g_vel_mode ? 1 : 0,
                     g_cruise_mode ? 1 : 0, g_cruise_dps, g_cruise_amp,
                     s_stm_yaw, (unsigned)s_stm_temp, (unsigned)s_stm_err,
                     (unsigned)s_stm_state, (unsigned long)s_stm_frames,
                     (unsigned long)st_age,
                     st_fresh ? 1 : 0, st_online ? 1 : 0,
                     s_motor_pos, s_motor_vel, s_motor_tau, (unsigned)s_motor_cf,
                     (int)s_espnow_channel,
                     ws_server_clients(),
                     (unsigned long)br_age, (unsigned)s_br_flags,
                     g_safe ? 1 : 0, (unsigned)g_safe_reason,
                     arm, arm_token, (unsigned long)s_br_arm_rej_total,
                     (unsigned long)g_ws_cmd_drop, (unsigned long)s_cmd_age_drop,
                     (unsigned long)sub_ok, (unsigned long)sub_fail,
                     (unsigned long)cb_ok, (unsigned long)cb_fail,
                     (unsigned long)confirm, (unsigned long)cb_to, cb_sync, enabled);
    return (m > 0 && (size_t)m < n) ? (size_t)m : 0;
}

void app_run_command(const char *line)
{
    if (line == NULL) {
        return;
    }
    /* handle_command 会就地改字符串 (去空白), 所以先拷到本地缓冲 */
    char tmp[64];
    size_t len = strlen(line);
    if (len >= sizeof(tmp)) {
        len = sizeof(tmp) - 1;
    }
    memcpy(tmp, line, len);
    tmp[len] = '\0';
    handle_command(tmp);
}

/* ================================================================== */
/* WebSocket 任务的命令投递 (跨核心, 走队列; 队列满就丢, 不阻塞)      */
/* ================================================================== */
/* 队列元素布局 (64 字节, 契约 §4.6 要求长度保持 64):
 *   item[0..57]  命令文本 (NUL 结尾)
 *   item[60..63] 入队时的 millis(), 出队时用于 100ms 过期判定
 * ★ 安全命令不走这里 (见 safety_request / s_safety_cmd 旁路槽)。 */
void app_run_command_async(const char *line)
{
    if (line == NULL) {
        return;
    }

    /* ★ 安全命令: 只写原子旁路槽, 不依赖队列, 队列满也绝不丢停止/急停命令。 */
    {
        uint8_t sc = safety_cmd_of(line);
        if (sc != SAFETY_NONE) {
            safety_request(sc);
            return;
        }
    }

    if (s_ws_cmd_q == nullptr) {
        g_ws_cmd_drop = g_ws_cmd_drop + 1u;
        return;
    }

    uint32_t epoch = __atomic_load_n(&s_safety_epoch, __ATOMIC_ACQUIRE);
    char item[WS_CMD_Q_ITEM];
    memset(item, 0, sizeof(item));
    size_t len = strlen(line);
    if (len > WS_CMD_TEXT_MAX) {
        len = WS_CMD_TEXT_MAX;
    }
    memcpy(item, line, len);
    item[WS_CMD_TEXT_MAX] = '\0';          /* 保证 handle_command 不会读到时间戳字节 */
    uint32_t ts = millis();
    memcpy(item + WS_CMD_TS_OFF, &ts, sizeof(ts));
    memcpy(item + WS_CMD_EPOCH_OFF, &epoch, sizeof(epoch));

    if (xQueueSend(s_ws_cmd_q, item, 0) != pdTRUE) {
        /* 队列满: 普通命令丢弃并计数 (q_drop), 绝不阻塞 WS 任务 */
        g_ws_cmd_drop = g_ws_cmd_drop + 1u;
    }
}

/* ================================================================== */
/* 参数预设 (网页/串口共用; 存在 NVS 里)                               */
/* ================================================================== */

size_t app_presets_json(char *buf, size_t n)
{
    size_t used = 0;
    int m = snprintf(buf, n, "[");
    if (m <= 0) {
        return 0;
    }
    used = (size_t)m;
    bool first = true;

    for (uint8_t i = 0; i < PRESET_SLOTS; i++) {
        preset_blob_t p;
        if (!preset_load(i, &p)) {
            continue;
        }
        m = snprintf(buf + used, n - used,
                     "%s{\"i\":%u,\"name\":\"%s\",\"kp\":%.1f,\"kd\":%.2f,"
                     "\"a\":%.0f,\"t\":%.1f,\"A\":%.0f,\"v\":%.0f,\"r\":%.0f}",
                     first ? "" : ",", (unsigned)i, p.name,
                     p.params.kp, p.params.kd,
                     p.params.sine_amp_deg, p.params.sine_period_s,
                     p.params.sweep_amp_deg, p.params.sweep_speed_dps,
                     p.params.circle_radius_deg);
        if (m <= 0 || used + (size_t)m >= n) {
            break;
        }
        used += (size_t)m;
        first = false;
    }

    if (used + 2 < n) {
        buf[used++] = ']';
        buf[used] = '\0';
    }
    return used;
}

bool app_preset_save(uint8_t slot, const char *name)
{
    param_blob_t b;
    params_fill(&b);
    return preset_save(slot, (name && name[0]) ? name : "preset", &b);
}

bool app_preset_load(uint8_t slot)
{
    preset_blob_t p;
    if (!preset_load(slot, &p)) {
        return false;
    }
    params_apply(&p.params);
    params_save_now();          /* 应用后也持久化, 掉电再上电还是这套 */
    return true;
}

bool app_param_request(uint8_t op, uint8_t rid, const uint8_t value[4])
{
    return param_request(op, rid, value);
}

size_t app_param_values_json(char *buf, size_t n)
{
    size_t used = 0;
    int m = snprintf(buf, n, "{");
    if (m <= 0) return 0;
    used = (size_t)m;
    bool first = true;
    for (int i = 0; i < PARAM_SLOTS; i++) {
        if (!s_param_have[i]) continue;
        uint32_t u = (uint32_t)s_param_val[i][0] | ((uint32_t)s_param_val[i][1] << 8)
                   | ((uint32_t)s_param_val[i][2] << 16) | ((uint32_t)s_param_val[i][3] << 24);
        float f;
        memcpy(&f, &u, 4);
        m = snprintf(buf + used, n - used,
                     "%s\"%d\":[%u,%.6g]", first ? "" : ",",
                     i, (unsigned)u, (double)f);
        if (m <= 0 || used + (size_t)m >= n) break;
        used += (size_t)m;
        first = false;
    }
    if (used + 2 < n) {
        buf[used++] = '}';
        buf[used] = '\0';
    }
    return used;
}

bool app_preset_clear(uint8_t slot)
{
    return preset_clear(slot);
}

/* ================================================================== */
/* setup / loop                                                       */
/* ================================================================== */

void setup(void)
{
    Serial.begin(SERIAL_BAUD);
    delay(300);

    Serial.println();
    Serial.println(F("=============================================="));
    Serial.println(F("  靶车 · ESP32 主机 (ESP-NOW 发射端)"));
    Serial.println(F("  链路: ESP32 --ESP-NOW--> M5Stamp S3 --UART--> STM32 --CAN--> DM-G6220"));
    Serial.println(F("=============================================="));

    motion_profile_init(&g_profile);
    motion_profile_step(&g_profile, 0.0f, &g_target);

    /* 从 Flash 恢复上次调好的参数 (只恢复 kp/kd/幅度这些, 不恢复使能位) */
    params_load();

    /* ★ v1.1 链路可靠性内核初始化 (必须在起 ESP-NOW 之前):
     *   - 授权令牌种子用 micros() 混合: 每次复位后令牌都不同, 保证 STM32 侧
     *     "旧令牌永不能重放" 这条不变量成立 (契约 §1.1);
     *   - 新鲜度/发送统计清零, 上电即"未收到任何帧", 不能把零值当成新鲜。 */
    {
        uint8_t seed = (uint8_t)(micros() ^ (micros() >> 8) ^ (micros() >> 16) ^
                                 (millis() << 3));
        lc_arm_tx_init(&g_arm, seed);
        lc_fresh_init(&g_stm_fresh);
        lc_fresh_init(&g_bridge_fresh);
        lc_txstats_init(&s_tx);
    }
    g_estop = false;
    g_safe  = false;
    g_safe_reason = SAFE_REASON_NONE;
    g_flags = 0;                     /* 上电默认失能, 且没有任何 ARM 令牌 */

    /* 起 WiFi: 热点立刻可用, 连家里路由器放后台 (不再阻塞开机) */
    uint8_t ch = wifi_start();

    if (!espnow_init(ch)) {
        Serial.println(F("[致命] ESP-NOW 初始化失败, 请检查板卡/固件版本"));
    }

    /* 打印一次复位原因, 稳定性排查时很有用 */
    {
        esp_reset_reason_t rr = esp_reset_reason();
        Serial.printf("[SYS] 复位原因 = %d, 空闲内存 = %u B\n",
                      (int)rr, (unsigned)ESP.getFreeHeap());
    }

#if WEB_UI_ENABLE
    /* WebSocket 任务(核心0)投递过来的命令队列: 必须在起 WS 之前建好 */
    s_ws_cmd_q = xQueueCreate(WS_CMD_Q_LEN, WS_CMD_Q_ITEM);
    if (s_ws_cmd_q == nullptr) {
        Serial.println(F("[WS] 命令队列创建失败 (WS 发命令会被丢弃)"));
    }

    /* 网页控制台 (热点已经在 wifi_start 里开好了, 这里只起 HTTP 服务) */
    web_ui_begin();
    /* WebSocket 推送 (端口 81): 20 Hz 推状态给实时曲线; 连不上会退回 HTTP 轮询 */
    ws_server_begin();
#endif

    /* 板载 ST7789 状态屏 (ESP32-S3-GEEK 自带): 失败不影响控制, 只打日志 */
    if (lcd_init()) {
        Serial.println(F("[LCD] ST7789 135x240 就绪 (板载屏, 背光 PWM 80%)"));
        lcd_show("TARGETCAR MASTER", "Booting...", "", "", "");
    } else {
        Serial.println(F("[LCD] 初始化失败: 屏幕不可用, 控制逻辑照常运行"));
    }

    print_help();

    g_last_loop_ms = millis();
    g_last_log_ms  = millis();
}

void loop(void)
{
    /* ---- 0. 最高优先级: 安全命令旁路槽 (x/C/R) ----
     *   ★ 契约 §4.1: 在串口/HTTP/WS 队列之前处理; WS 任务写槽不依赖队列,
     *   所以即使命令队列满, 停止/急停命令也一定会被执行。 */
    poll_safety_bypass();

    uint32_t now = millis();

    /* ---- 0b. ESP-NOW 在途超时统计 + SAFE 状态机 (契约 §4.3/§4.4) ----
     *   每轮都过期一次在途表; 连续在途超时 (>=3) 会作为 SAFE 的原因之一。 */
    {
        uint32_t expired;
        tx_lock();
        expired = lc_txstats_expire(&s_tx, now, ESPNOW_CB_TIMEOUT_MS);
        tx_unlock();
        if (expired > 0u) {
            s_cb_timeout_streak += expired;
            auth_drop();
            motion_modes_stop();
            (void)clear_ws_cmd_queue();
        } else if (lc_fresh_ok(&g_bridge_fresh, now, MASTER_BRIDGE_STALE_MS)) {
            s_cb_timeout_streak = 0;
        }
    }
    safe_state_eval(now);

    /* ---- 0c. WebSocket 任务投递过来的普通命令 (跨核心安全) ----
     *   ★ 出队时按 100ms 过期丢弃: 循环被 WiFi/HTTP 拖慢时, 旧命令不能当新命令执行。 */
#if WEB_UI_ENABLE
    if (s_ws_cmd_q != nullptr) {
        char item[WS_CMD_Q_ITEM];
        while (xQueueReceive(s_ws_cmd_q, item, 0) == pdTRUE) {
            poll_safety_bypass();
            uint32_t epoch = 0;
            memcpy(&epoch, item + WS_CMD_EPOCH_OFF, sizeof(epoch));
            if (epoch != __atomic_load_n(&s_safety_epoch, __ATOMIC_ACQUIRE)) continue;
            item[WS_CMD_TEXT_MAX] = '\0';          /* 命令文本与时间戳之间强制截断 */
            uint32_t ts = 0;
            memcpy(&ts, item + WS_CMD_TS_OFF, sizeof(ts));
            if (lc_age_exceeds(now, ts, CTRL_CMD_MAX_AGE_MS)) {
                s_cmd_age_drop = s_cmd_age_drop + 1u;
                continue;
            }
            app_run_command(item);
        }
    }
#endif

    /* ---- 1. 串口命令 ---- */
    poll_serial();

    /* ---- 1b. 网页控制台的 HTTP 请求 (非阻塞, 每次只处理一点) ---- */
#if WEB_UI_ENABLE
    web_ui_poll();
#endif

    poll_safety_bypass(); /* Apply serial/HTTP/concurrent WS stops before transmitting. */

    /* ---- 2. 定周期控制循环 ---- */
    uint32_t dt_ms = now - g_last_loop_ms;
    if (dt_ms >= CTRL_LOOP_PERIOD_MS) {
        float dt_s = (float)dt_ms / 1000.0f;
        g_last_loop_ms = now;

        /* 巡航: 固定速度往返, 到达 ±幅度就反向 */
        if (g_cruise_mode) {
            float step = g_cruise_dps * dt_s;
            g_profile.manual_yaw_deg += (float)g_cruise_dir * step;
            if (g_profile.manual_yaw_deg >= g_cruise_amp) {
                g_profile.manual_yaw_deg = g_cruise_amp;
                g_cruise_dir = -1;
            } else if (g_profile.manual_yaw_deg <= -g_cruise_amp) {
                g_profile.manual_yaw_deg = -g_cruise_amp;
                g_cruise_dir = 1;
            }
            motion_profile_set_manual(&g_profile,
                                      g_profile.manual_yaw_deg,
                                      g_profile.manual_pitch_deg);
        }

        /* 速度模式: 把角速度积分成目标角 (限位内), 再下发给轨迹器 */
        if (g_vel_mode) {
            g_profile.manual_yaw_deg = proto_clampf(
                g_profile.manual_yaw_deg + g_vel_yaw_dps * dt_s,
                GIMBAL_YAW_MIN_DEG, GIMBAL_YAW_MAX_DEG);
            g_profile.manual_pitch_deg = proto_clampf(
                g_profile.manual_pitch_deg + g_vel_pitch_dps * dt_s,
                GIMBAL_PITCH_MIN_DEG, GIMBAL_PITCH_MAX_DEG);
            motion_profile_set_manual(&g_profile,
                                      g_profile.manual_yaw_deg,
                                      g_profile.manual_pitch_deg);
        }

        /* 生成目标 */
        motion_profile_step(&g_profile, dt_s, &g_target);

        if (g_vel_mode) {
            /* 速度前馈: 让 MIT 目标速度跟上, 电机才真的匀速转 */
            g_target.yaw_rate_dps   = g_vel_yaw_dps;
            g_target.pitch_rate_dps = g_vel_pitch_dps;
        } else if (g_cruise_mode) {
            g_target.yaw_rate_dps   = (float)g_cruise_dir * g_cruise_dps;
            g_target.pitch_rate_dps = 0.0f;
        }

        /* 一次性标志用完即清 (清错/零点只需发一次) */
        uint16_t tx_flags = g_flags;
        if (tx_flags & (CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO)) {
            g_flags &= (uint16_t)~(CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO);
        }

        /* 编码 + 发包 (契约 §4.4: 提交结果只记 submit, 不当作"已送达") */
        uint8_t buf[CTRL_FRAME_SIZE];
        uint8_t seq_now = g_seq;
        proto_encode_ctrl(buf, &g_target, tx_flags, g_motor_mode, seq_now);
        (void)radio_send(LC_TX_CTRL, seq_now, buf, sizeof(buf));
        g_seq = (uint8_t)(g_seq + 1u);
    }

    /* ---- 2a2. 单播自愈 (契约 §4.9): 判据改为 lc_txstats 回调 + 桥接新鲜度,
     *   不再用 "3000ms 无回传 + 40 帧失败" 这两个魔法数:
     *   同一 2 秒窗口内, 桥接新鲜度帧已经过期、控制帧回调没有任何成功、却有失败
     *   -> 认为对端 MAC 不可达, 退回广播。 */
    if (!g_use_bcast) {
        static uint32_t s_fb_chk_ms      = 0;
        static uint32_t s_cb_ok_snap     = 0;
        static uint32_t s_cb_fail_snap   = 0;
        if ((uint32_t)(now - s_fb_chk_ms) > 2000u) {
            uint32_t cb_ok_now, cb_fail_now;
            tx_lock();
            cb_ok_now   = s_tx.cb_ok[LC_TX_CTRL];
            cb_fail_now = s_tx.cb_fail[LC_TX_CTRL];
            tx_unlock();

            bool bridge_stale = !lc_fresh_ok(&g_bridge_fresh, now, MASTER_BRIDGE_STALE_MS);
            bool no_cb_ok     = (cb_ok_now == s_cb_ok_snap);
            bool had_cb_fail  = (cb_fail_now > s_cb_fail_snap);

            if (bridge_stale && no_cb_ok && had_cb_fail) {
                uint8_t bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
                esp_now_del_peer(g_peer_mac);
                esp_now_peer_info_t p2;
                memset(&p2, 0, sizeof(p2));
                memcpy(p2.peer_addr, bcast, 6);
                p2.channel = 0;
                p2.encrypt = false;
                if (esp_now_add_peer(&p2) == ESP_OK) {
                    memcpy(g_peer_mac, bcast, 6);
                    g_use_bcast = true;
                    Serial.println(F("[ESP-NOW] 单播无任何回调确认且桥接帧过期, 已退回广播模式"));
                }
            }
            s_fb_chk_ms    = now;
            s_cb_ok_snap   = cb_ok_now;
            s_cb_fail_snap = cb_fail_now;
        }
    }

    /* ---- 2b. 增益下发: 改了立刻发, 没改也每 1 s 发一次 (容忍丢包) ----
     *   增益/参数帧记 LC_TX_PARAM, 不和控制帧串账 (契约 §4.4)。 */
    if (s_gains_dirty || (uint32_t)(now - s_gains_last_ms) >= 1000u) {
        uint8_t gbuf[GAINS_FRAME_SIZE];
        uint8_t gseq = s_gains_seq;
        proto_encode_gains(gbuf, g_kp, g_kd, gseq);
        bool g_ok = radio_send(LC_TX_PARAM, gseq, gbuf, sizeof(gbuf));
        if (g_ok) {
            s_gains_seq = (uint8_t)(s_gains_seq + 1u);
            s_gains_dirty = false;
            s_gains_last_ms = now;
        }

    }

    /* ---- 2c. WiFi 后台维护 (STA 重连 / 信道跟随), 不阻塞 ---- */
    wifi_poll();

    /* ---- 2d. 板载屏状态刷新 (5 Hz, 失败自动跳过) ---- */
    if ((uint32_t)(now - s_lcd_last_ms) >= LCD_UPDATE_MS) {
        s_lcd_last_ms = now;

        char b0[20], b1[20], b2[20], b3[20], b4[20], b5[20], b6[20], b7[20], b8[20];
        bool en = (g_flags & CTRL_FLAG_ENABLE) && !g_estop && !g_safe;
        const char *prof = motion_profile_name(g_profile.id);

        /* 统计只取回调确认结果, 不显示"提交成功" (契约 §4.4) */
        uint32_t lcd_cb_ok, lcd_cb_fail;
        tx_lock();
        lcd_cb_ok   = s_tx.cb_ok[LC_TX_CTRL];
        lcd_cb_fail = s_tx.cb_fail[LC_TX_CTRL];
        tx_unlock();

        /* 10x14 字模: 每行 12 个字符 */
        snprintf(b0, sizeof(b0), "MASTER %s",
                 g_safe ? "SAFE" : (g_estop ? "STOP" : (en ? "ON" : "OFF")));
        snprintf(b1, sizeof(b1), "%s", prof);
        snprintf(b2, sizeof(b2), "K%4.1f D%.2f", g_kp, g_kd);
        snprintf(b3, sizeof(b3), "Y %6.2f", g_target.yaw_deg);
        snprintf(b4, sizeof(b4), "P %5.1f", g_target.pitch_deg);
        if (WiFi.status() == WL_CONNECTED) {
            snprintf(b5, sizeof(b5), "%s", WiFi.localIP().toString().c_str());
        } else {
            snprintf(b5, sizeof(b5), "192.168.4.1");
        }
        snprintf(b6, sizeof(b6), "CH %d", (int)s_espnow_channel);
        snprintf(b7, sizeof(b7), "CB %lu/%lu",
                 (unsigned long)lcd_cb_ok, (unsigned long)lcd_cb_fail);
        snprintf(b8, sizeof(b8), "UP %lus", (unsigned long)(now / 1000u));

        const char *lines9[9] = { b0, b1, b2, b3, b4, b5, b6, b7, b8 };
        lcd_show_lines(lines9, 9);
    }

    /* ---- 3. 遥测 ---- */
#if MASTER_ENABLE_TELEMETRY_LOG
    if (now - g_last_log_ms >= TELEMETRY_LOG_PERIOD_MS) {
        g_last_log_ms = now;
        print_status();
    }
#endif

    /* ---- 4. 参数自动落盘: 改完 1.5 s 内没有新改动就写一次 Flash ---- */
    if (s_param_dirty && (now - s_param_dirty_ms) >= PARAM_SAVE_DELAY_MS) {
        params_save_now();
    }

    if (s_reboot_pending && (int32_t)(millis() - s_reboot_at_ms) >= 0) ESP.restart();
    delay(1);
}
