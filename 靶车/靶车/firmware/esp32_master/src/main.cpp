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
 *     x          急停 (持续发 ESTOP)
 *     R          恢复 (解除急停)
 *     s          立即打印一次状态
 */

#include <Arduino.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

#include "protocol.h"
#include "app_config.h"
#include "motion_profile.h"

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
static uint16_t          g_flags        = CTRL_FLAG_ENABLE;
static uint8_t           g_motor_mode   = MOTOR_MODE_MIT;
static float             g_kp           = GIMBAL_KP_DEFAULT;
static float             g_kd           = GIMBAL_KD_DEFAULT;
static bool              g_estop        = false;

static uint32_t          g_tx_ok        = 0;
static uint32_t          g_tx_fail      = 0;
static uint32_t          g_last_loop_ms = 0;
static uint32_t          g_last_log_ms  = 0;
static char              g_line[64];
static uint8_t           g_line_len     = 0;

/* ESP-NOW 回执 */
static volatile bool     g_last_send_ok = false;

/* ================================================================== */
/* 工具                                                               */
/* ================================================================== */

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
    Serial.println(F("  e           清电机错误      z 保存零点   x 急停   R 恢复"));
    Serial.println(F("  s           打印一次状态    m 本帮助"));
    Serial.println(F("========================================="));
}

static void print_status(void)
{
    Serial.printf("[%8lu] 轨迹=%-7s yaw=%7.2f pitch=%7.2f  "
                  "rate=(%6.1f,%6.1f) kp=%.1f kd=%.2f  TX ok=%lu fail=%lu %s\n",
                  (unsigned long)millis(),
                  motion_profile_name(g_profile.id),
                  g_target.yaw_deg, g_target.pitch_deg,
                  g_target.yaw_rate_dps, g_target.pitch_rate_dps,
                  g_kp, g_kd,
                  (unsigned long)g_tx_ok, (unsigned long)g_tx_fail,
                  g_estop ? "【急停】" : "");
}

/* ================================================================== */
/* ESP-NOW                                                            */
/* ================================================================== */

static uint8_t g_peer_mac[6] = {
    ESPNOW_PEER_MAC_0, ESPNOW_PEER_MAC_1, ESPNOW_PEER_MAC_2,
    ESPNOW_PEER_MAC_3, ESPNOW_PEER_MAC_4, ESPNOW_PEER_MAC_5
};

static void on_sent(const uint8_t *mac_addr, esp_now_send_status_t status)
{
    (void)mac_addr;
    g_last_send_ok = (status == ESP_NOW_SEND_SUCCESS);
}

#if ESP_ARDUINO_VERSION_MAJOR >= 3
/* core 3.x 的发送回调签名不一样 (第一个参数是 wifi_tx_info_t*) */
static void on_sent_cb(const wifi_tx_info_t *info, esp_now_send_status_t status)
{
    (void)info;
    on_sent(nullptr, status);
}
#endif

static bool espnow_init(void)
{
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();

    /* 锁死信道: ESP-NOW 与 WiFi 共用射频, 信道漂移会直接断链 */
    if (esp_wifi_set_channel(ESPNOW_CHANNEL, WIFI_SECOND_CHAN_NONE) != ESP_OK) {
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
#else
    esp_now_register_send_cb(on_sent);
#endif

    esp_now_peer_info_t peer;
    memset(&peer, 0, sizeof(peer));
    memcpy(peer.peer_addr, g_peer_mac, 6);
    peer.channel = ESPNOW_CHANNEL;
    peer.encrypt = false;
    peer.ifidx   = WIFI_IF_STA;

    if (!esp_now_is_peer_exist(g_peer_mac)) {
        if (esp_now_add_peer(&peer) != ESP_OK) {
            Serial.println(F("[ESP-NOW] 添加对端失败!"));
            return false;
        }
    }

    Serial.printf("[ESP-NOW] 就绪, 信道=%d, 本机MAC=%s\n",
                  ESPNOW_CHANNEL, WiFi.macAddress().c_str());
    Serial.printf("[ESP-NOW] 对端MAC=%02X:%02X:%02X:%02X:%02X:%02X %s\n",
                  g_peer_mac[0], g_peer_mac[1], g_peer_mac[2],
                  g_peer_mac[3], g_peer_mac[4], g_peer_mac[5],
                  ESPNOW_USE_BROADCAST ? "(广播)" : "(定向)");
    return true;
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

    char   cmd = line[0];
    char  *arg = line + 1;
    while (*arg == ' ') arg++;
    float  val = (float)atof(arg);

    switch (cmd) {

    /* ---- 轨迹切换 ---- */
    case '0': case '1': case '2': case '3': case '4': case '5':
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
        motion_profile_set_manual(&g_profile,
                                  g_profile.manual_yaw_deg,
                                  proto_clampf(val, GIMBAL_PITCH_MIN_DEG, GIMBAL_PITCH_MAX_DEG));
        if (g_profile.id != PROFILE_MANUAL) {
            motion_profile_set(&g_profile, PROFILE_MANUAL,
                               g_target.yaw_deg, g_target.pitch_deg);
        }
        Serial.printf(">> 手动俯仰 = %.2f 度\n", g_profile.manual_pitch_deg);
        break;

    /* ---- 增益 ---- */
    case 'k':
        g_kp = proto_clampf(val, 0.0f, 500.0f);
        Serial.printf(">> kp = %.1f\n", g_kp);
        break;

    case 'd':
        g_kd = proto_clampf(val, 0.0f, 5.0f);
        Serial.printf(">> kd = %.2f\n", g_kd);
        break;

    /* ---- 一次性动作 ---- */
    case 'e':
        g_flags |= CTRL_FLAG_CLEAR_ERR;
        Serial.println(F(">> 发送清错标志 (下一帧生效)"));
        break;

    case 'z':
        g_flags |= CTRL_FLAG_SET_ZERO;
        Serial.println(F(">> 发送保存零点标志! 确认当前位置就是要作为零点的位置!"));
        break;

    case 'x':
        g_estop = true;
        g_flags |= CTRL_FLAG_ESTOP;
        Serial.println(F(">> 【急停】持续下发 ESTOP"));
        break;

    case 'R':
        g_estop = false;
        g_flags &= (uint16_t)~CTRL_FLAG_ESTOP;
        Serial.println(F(">> 解除急停"));
        break;

    case 's':
        print_status();
        break;

    case 'm':
    case 'h':
    case '?':
        print_help();
        break;

    default:
        Serial.printf("?? 未知命令 '%c', 输入 m 看帮助\n", cmd);
        break;
    }
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

    if (!espnow_init()) {
        Serial.println(F("[致命] ESP-NOW 初始化失败, 请检查板卡/固件版本"));
    }

    print_help();

    g_last_loop_ms = millis();
    g_last_log_ms  = millis();
}

void loop(void)
{
    /* ---- 1. 串口命令 ---- */
    poll_serial();

    /* ---- 2. 定周期控制循环 ---- */
    uint32_t now = millis();
    uint32_t dt_ms = now - g_last_loop_ms;

    if (dt_ms >= CTRL_LOOP_PERIOD_MS) {
        float dt_s = (float)dt_ms / 1000.0f;
        g_last_loop_ms = now;

        /* 生成目标 */
        motion_profile_step(&g_profile, dt_s, &g_target);

        /* 一次性标志用完即清 (清错/零点只需发一次) */
        uint16_t tx_flags = g_flags;
        if (tx_flags & (CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO)) {
            g_flags &= (uint16_t)~(CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO);
        }

        /* 编码 + 发包 */
        uint8_t buf[CTRL_FRAME_SIZE];
        proto_encode_ctrl(buf, &g_target, tx_flags, g_motor_mode, g_seq++);

        if (esp_now_send(g_peer_mac, buf, sizeof(buf)) == ESP_OK && g_last_send_ok) {
            g_tx_ok++;
        } else {
            g_tx_fail++;
        }
    }

    /* ---- 3. 遥测 ---- */
#if MASTER_ENABLE_TELEMETRY_LOG
    if (now - g_last_log_ms >= TELEMETRY_LOG_PERIOD_MS) {
        g_last_log_ms = now;
        print_status();
    }
#endif

    delay(1);
}
