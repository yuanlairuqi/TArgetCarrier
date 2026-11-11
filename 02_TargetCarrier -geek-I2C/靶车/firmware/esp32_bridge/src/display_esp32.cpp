/**
 * @file    display_esp32.cpp
 * @brief   M5Stamp S3 桥接端的 OLED 状态显示
 *
 *  屏幕布局 (128x64, 3x5 字库, 每个字符占 4x8 像素):
 *
 *    ┌────────────────────────────────┐
 *    │BRIDGE S3BAT            RUN     │  ← 标题 + 安全锁存 (RUN/FAIL/DEGR)
 *    │LINK  OK    rssi -45           │  ← 主机在线状态 (放大 2 倍) + 信号强度
 *    │rx 1234     ng 0    dp 0       │  ← 收包统计 (有效/异常/重复)
 *    │STM  -28.43  T28C  E--         │  ← STM32 回传角度 / 温度 / 电机状态码
 *    │age 12ms    ovf 0   OK         │  ← 反馈年龄(或 STALE) + 串口溢出 + 链路状态
 *    │────────────────────────────────│
 *    │fwd 1234   fs 0        12s     │  ← 转发数 / 失能帧注入数 / 运行秒数
 *    └────────────────────────────────┘
 *
 *  为什么新增"STM32 反馈年龄"这一行:
 *    桥接现在只在 STM32 反馈新鲜 (<=300ms) 时把 STATUS 转给主机, 过期后不再重发
 *    最后一帧缓存。旧屏幕只要收到过一帧就永远显示角度, 现场无法区分"真在线"和
 *    "早掉线了、主机看到的是旧数据"。所以这里必须显示:
 *      age xxms  = 反馈新鲜 (xx = 真实年龄)
 *      STALE     = 反馈已过期 (超过 BRIDGE_STM_FRESH_MS)
 *      age --    = 本次上电还没收到过有效 STATUS
 *  右上角的 RUN/FAIL/DEGR 是安全锁:
 *      RUN  = 正常转发
 *      FAIL = FAILSAFE 锁存, 正在注入失能帧, 不转发 ENABLE (需主机重新授权)
 *      DEGR = 初始化降级安全态 (队列/任务/上行 peer 失败), 同上但不自动恢复
 */

#include <Arduino.h>

#include "display.h"
#include "oled.h"
#include "oled_hal_esp32.h"
#include "bridge_status.h"
#include "app_config.h"

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

/* 计数值只显示低 4 位: 128px 的屏幕放不下 10 位十进制, 现场看趋势足够 */
static unsigned long low4(uint32_t v)
{
    return (unsigned long)(v % 10000u);
}

static void render(void)
{
    bridge_status_t st;
    bridge_get_status(&st);
    char buf[32];

    oled_clear();

    /* ---- 标题 + 安全锁 ---- */
    oled_text(0, 0, "BRIDGE S3BAT", 1);
    const char *lock = st.degraded ? "DEGR" : (st.motion_blocked ? "FAIL" : "RUN ");
    oled_text(104, 0, lock, 1);

    /* ---- 主机链路状态 (放大 2 倍, 最重要) ---- */
    oled_text(0, 8, "LINK", 1);
    oled_text(28, 8, st.link_up ? "OK" : "LOST", 2);

    /* ---- RSSI (2.x 核心回调没有 RSSI, 0 = 未知, 不是"信号很好") ---- */
    snprintf(buf, sizeof(buf), "rssi %d", (int)st.rssi);
    oled_text(78, 10, buf, 1);

    /* ---- 收包统计 ---- */
    snprintf(buf, sizeof(buf), "rx %lu", low4(st.rx_ok));
    oled_text(0, 26, buf, 1);

    snprintf(buf, sizeof(buf), "ng %lu", low4(st.rx_bad));
    oled_text(44, 26, buf, 1);

    snprintf(buf, sizeof(buf), "dp %lu", low4(st.rx_dup));
    oled_text(88, 26, buf, 1);

    /* ---- STM32 回传角度 / 温度 / 电机状态码 ---- */
    oled_text(0, 34, "STM", 1);
    if (st.have_stm32) {
        fmt_fixed(buf, sizeof(buf), st.stm32_yaw_deg, 2);
    } else {
        snprintf(buf, sizeof(buf), "--.--");
    }
    oled_text(20, 34, buf, 1);

    snprintf(buf, sizeof(buf), "T%uC", (unsigned)st.stm32_temp);
    oled_text(68, 34, buf, 1);

    /* ERR 是电机状态码 (0=失能 1=使能), 只有 >=0x8 才是故障, 正常时不显示数字 */
    if (st.stm32_err >= 0x8u) {
        snprintf(buf, sizeof(buf), "E%X", (unsigned)st.stm32_err);
    } else {
        snprintf(buf, sizeof(buf), "E--");
    }
    oled_text(100, 34, buf, 1);

    /* ---- STM32 反馈年龄 (新鲜/过期) / 串口溢出 / 链路状态 ---- */
    if (!st.have_stm32) {
        snprintf(buf, sizeof(buf), "age --");
    } else if (st.stm32_fresh) {
        uint32_t age = st.stm32_age_ms;
        if (age > 999u) {
            age = 999u;
        }
        snprintf(buf, sizeof(buf), "age %lums", (unsigned long)age);
    } else {
        /* 超过 BRIDGE_STM_FRESH_MS: 明确显示"过期", 不能继续显示成实时 */
        snprintf(buf, sizeof(buf), "STALE");
    }
    oled_text(0, 42, buf, 1);

    snprintf(buf, sizeof(buf), "ovf %lu", low4(st.overflows));
    oled_text(62, 42, buf, 1);

    oled_text(104, 42, st.have_stm32 ? link_state_text(st.stm32_state) : "NOF ", 1);

    /* ---- 分隔线 ---- */
    oled_hline(0, 50, OLED_WIDTH);

    /* ---- 转发 / 失能注入 / 运行秒数 ---- */
    snprintf(buf, sizeof(buf), "fwd %lu", low4(st.fwd_ok));
    oled_text(0, 56, buf, 1);

    snprintf(buf, sizeof(buf), "fs %lu", low4(st.failsafe_sent));
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
