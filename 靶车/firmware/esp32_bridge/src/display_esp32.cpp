/**
 * @file    display_esp32.cpp
 * @brief   M5Stamp S3 桥接端的 OLED 状态显示
 *
 *  屏幕布局 (128x64, 3x5 字库):
 *
 *    ┌────────────────────────────────┐
 *    │ BRIDGE  S3BAT                  │  ← 标题
 *    │■ LINK OK     rssi -45         │  ← 主机在线状态 + 信号强度
 *    │■ rx 1234  ng 0  dup 0         │  ← 收包统计
 *    │────────────────────────────────│
 *    │STM  yaw  28.43                │  ← STM32 回传角度
 *    │T 28C  E00  OK                 │  ← 温度/错误码/状态
 *    │────────────────────────────────│
 *    │ fwd 1234   fs 0                │  ← 转发数 / 失能帧注入数
 *    └────────────────────────────────┘
 */

#include <Arduino.h>

#include "display.h"
#include "oled.h"
#include "oled_hal_esp32.h"
#include "bridge_status.h"

#include <stdio.h>

/* 刷新周期: 10Hz 足够看, 又不会占太多软件 I2C 时间 */
#define DISPLAY_PERIOD_MS   100u

static bool     s_ready   = false;
static uint32_t s_last_ms = 0;

/* 链接状态枚举 -> 短文本 (与 protocol.h 的 proto_link_state_t 对应) */
static const char *link_state_text(uint8_t state)
{
    switch (state) {
    case 0:  return "INIT";
    case 1:  return "OK  ";
    case 2:  return "TOUT";
    case 3:  return "STOP";
    case 4:  return "FLT ";
    default: return "?   ";
    }
}

static void fmt_fixed(char *buf, size_t n, float v, uint8_t decimals)
{
    if (n == 0) {
        return;
    }
    char sign = ' ';
    if (v < 0.0f) {
        sign = '-';
        v = -v;
    }
    long scale = 1;
    for (uint8_t i = 0; i < decimals; i++) {
        scale *= 10;
    }
    long scaled = (long)(v * (float)scale + 0.5f);
    long ip     = scaled / scale;
    long frac   = scaled % scale;

    if (decimals == 0) {
        snprintf(buf, n, "%c%ld", sign, ip);
    } else if (decimals == 1) {
        snprintf(buf, n, "%c%ld.%01ld", sign, ip, frac);
    } else {
        snprintf(buf, n, "%c%ld.%02ld", sign, ip, frac);
    }
}

static void render(void)
{
    bridge_status_t st;
    bridge_get_status(&st);
    char buf[32];

    oled_clear();

    /* ---- 标题 ---- */
    oled_text(0, 0, "BRIDGE S3BAT", 1);

    /* ---- 主机链路状态 (放大 2 倍, 最重要) ---- */
    oled_text(0, 8, "LINK", 1);
    oled_text(18, 8, st.link_up ? "OK" : "LOST", 2);

    /* ---- RSSI ---- */
    snprintf(buf, sizeof(buf), "rssi %d", (int)st.rssi);
    oled_text(70, 8, buf, 1);

    /* ---- 收包统计 ---- */
    snprintf(buf, sizeof(buf), "rx %lu", (unsigned long)st.rx_ok);
    oled_text(0, 20, buf, 1);

    snprintf(buf, sizeof(buf), "ng %lu", (unsigned long)st.rx_bad);
    oled_text(44, 20, buf, 1);

    snprintf(buf, sizeof(buf), "dp %lu", (unsigned long)st.rx_dup);
    oled_text(88, 20, buf, 1);

    /* ---- 分隔线 ---- */
    oled_hline(0, 28, OLED_WIDTH);

    /* ---- STM32 回传角度 (放大 2 倍) ---- */
    oled_text(0, 32, "STM", 1);
    if (st.have_stm32) {
        fmt_fixed(buf, sizeof(buf), st.stm32_yaw_deg, 2);
    } else {
        snprintf(buf, sizeof(buf), "--.--");
    }
    oled_text(18, 32, buf, 2);

    /* ---- 温度 / 错误码 / 链路状态 ---- */
    snprintf(buf, sizeof(buf), "T%uC", (unsigned)st.stm32_temp);
    oled_text(0, 44, buf, 1);

    snprintf(buf, sizeof(buf), "E%02u", (unsigned)st.stm32_err);
    oled_text(34, 44, buf, 1);

    oled_text(70, 44, st.have_stm32 ? link_state_text(st.stm32_state) : "NOF  ", 1);

    /* ---- 分隔线 ---- */
    oled_hline(0, 52, OLED_WIDTH);

    /* ---- 转发 / 失能注入 ---- */
    snprintf(buf, sizeof(buf), "fwd %lu", (unsigned long)st.fwd_ok);
    oled_text(0, 56, buf, 1);

    snprintf(buf, sizeof(buf), "fs %lu", (unsigned long)st.failsafe_sent);
    oled_text(60, 56, buf, 1);

    snprintf(buf, sizeof(buf), "s%02lu", (unsigned long)((st.uptime_ms / 1000u) % 100u));
    oled_text(105, 56, buf, 1);

    oled_flush();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

bool display_init(void)
{
    oled_hal_esp32_gpio_init();

    /* 先试 0x3C (最常见), 不行再试 0x3D */
    s_ready = oled_init(oled_hal_esp32(), OLED_CTRL_SSD1306, OLED_I2C_ADDR_3C);
    if (!s_ready) {
        s_ready = oled_init(oled_hal_esp32(), OLED_CTRL_SSD1306, OLED_I2C_ADDR_3D);
    }
    if (!s_ready) {
        return false;
    }

    /* 开机画面 */
    oled_clear();
    oled_text(0, 12, "TARGET VEHICLE", 1);
    oled_text(0, 24, "BRIDGE M5STAMP", 1);
    oled_text(0, 36, "ESP-NOW -> UART", 1);
    oled_text(0, 48, "G9=SCL G8=SDA", 1);
    oled_flush();

    s_last_ms = millis();
    return true;
}

void display_tick(uint32_t now_ms)
{
    if (!s_ready) {
        return;
    }
    if ((uint32_t)(now_ms - s_last_ms) < DISPLAY_PERIOD_MS) {
        return;
    }
    s_last_ms = now_ms;
    render();
}

bool display_available(void)
{
    return s_ready;
}
