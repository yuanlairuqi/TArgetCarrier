/**
 * @file    display_stm32.c
 * @brief   STM32F103 的 OLED 状态显示内容
 *
 *  屏幕布局 (128x64, 3x5 字库):
 *
 *    ┌────────────────────────────────┐
 *    │ GIMBAL STM32                   │  ← 标题
 *    │■ TGT  30.00                    │  ← 目标角度 (放大2倍)
 *    │■ ACT  28.43                    │  ← 实际角度 (放大2倍)
 *    │────────────────────────────────│
 *    │EN  kp72 kd1.2                  │  ← 使能/增益
 *    │T 28C E00  FBK                  │  ← 温度/错误码/反馈
 *    │────────────────────────────────│
 *    │         ┃                      │  ← 角度指示条 (-180..+180)
 *    │   -180   ▲   0       +180      │
 *    └────────────────────────────────┘
 */

#include "display.h"
#include "oled.h"
#include "app_main.h"

#include <stdio.h>
#include <string.h>

/* 由 oled_hal_stm32.c 提供 */
const oled_hal_t *oled_hal_stm32(void);
void oled_hal_stm32_gpio_init(void);

/* 刷新周期: 10Hz 足够看, 不给主循环添负担 */
#define DISPLAY_PERIOD_MS   100u

static bool     s_ready = false;
static uint32_t s_last_ms = 0;

/* ------------------------------------------------------------------ */
/* 小工具                                                             */
/* ------------------------------------------------------------------ */

/* 把浮点数格式化成 "12.34" / "-5.00" 这种不带符号溢出的字符串 */
static void fmt_fixed(char *buf, size_t n, float v, uint8_t decimals)
{
    if (n == 0) {
        return;
    }

    /* 单独的负号处理: 避免 -0.001 显示成 "0.00" 或出现 "--" */
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

/* ------------------------------------------------------------------ */
/* 界面绘制                                                           */
/* ------------------------------------------------------------------ */

static void draw_angle_bar(int16_t y, float deg)
{
    /* 条: x 从 8 到 119, 对应 -180..+180 度 */
    const int16_t x0 = 8;
    const int16_t x1 = 119;
    const int16_t mid = (x0 + x1) / 2;

    oled_hline(x0, y, x1 - x0 + 1);

    /* 中点刻度 */
    for (int16_t i = 0; i < 3; i++) {
        oled_pixel(mid, (int16_t)(y - 1 - i), true);
    }

    /* 角度指针 */
    float clamped = deg;
    if (clamped < -180.0f) clamped = -180.0f;
    if (clamped >  180.0f) clamped =  180.0f;
    int16_t px = (int16_t)(mid + clamped / 180.0f * (float)(x1 - mid));

    for (int16_t i = 1; i <= 4; i++) {
        oled_pixel(px, (int16_t)(y - i), true);
    }
    oled_pixel((int16_t)(px - 1), (int16_t)(y - 1), true);
    oled_pixel((int16_t)(px + 1), (int16_t)(y - 1), true);
}

static void render(void)
{
    app_status_t st;
    app_get_status(&st);
    char buf[24];

    oled_clear();

    /* ---- 标题 ---- */
    oled_text(0, 0, "GIMBAL STM32", 1);

    /* ---- 目标角度 (放大 2 倍, 醒目) ---- */
    fmt_fixed(buf, sizeof(buf), st.target_deg, 2);
    oled_text(0, 8,  "TGT", 1);
    oled_text(18, 8, buf, 2);

    /* ---- 实际角度 (放大 2 倍) ---- */
    fmt_fixed(buf, sizeof(buf), st.actual_deg, 2);
    oled_text(0, 20, "ACT", 1);
    oled_text(18, 20, buf, 2);

    /* ---- 分隔线 ---- */
    oled_hline(0, 34, OLED_WIDTH);

    /* ---- 状态行 1: 使能 / 增益 ---- */
    {
        const char *en = st.estop ? "ESTOP"
                       : st.fault ? "FLT  "
                       : st.enabled ? "EN   "
                       : "DIS  ";
        oled_text(0, 38, en, 1);

        fmt_fixed(buf, sizeof(buf), st.kp, 0);
        oled_text(30, 38, "kp", 1);
        oled_text(40, 38, buf, 1);

        fmt_fixed(buf, sizeof(buf), st.kd, 1);
        oled_text(66, 38, "kd", 1);
        oled_text(76, 38, buf, 1);
    }

    /* ---- 状态行 2: 温度 / 错误码 / 反馈 ---- */
    {
        snprintf(buf, sizeof(buf), "T%uC", (unsigned)st.temp_mos);
        oled_text(0, 46, buf, 1);

        snprintf(buf, sizeof(buf), "E%02u", (unsigned)st.err_code);
        oled_text(40, 46, buf, 1);

        oled_text(76, 46, st.feedback_valid ? "FBK" : "NOF", 1);
    }

    /* ---- 角度指示条 ---- */
    draw_angle_bar(58, st.actual_deg);
    oled_text(0, 60, "-180", 1);
    oled_text(56, 60, "0", 1);
    oled_text(110, 60, "180", 1);

    oled_flush();
}

/* ------------------------------------------------------------------ */
/* 对外接口                                                           */
/* ------------------------------------------------------------------ */

bool display_init(void)
{
    oled_hal_stm32_gpio_init();

    /* 先试 0x3C (最常见), 不行再试 0x3D */
    s_ready = oled_init(oled_hal_stm32(), OLED_CTRL_SSD1306, OLED_I2C_ADDR_3C);
    if (!s_ready) {
        s_ready = oled_init(oled_hal_stm32(), OLED_CTRL_SSD1306, OLED_I2C_ADDR_3D);
    }

    if (!s_ready) {
        return false;
    }

    /* 开机画面: 显示固件标识 1 秒 */
    oled_clear();
    oled_text(0, 16, "TARGET VEHICLE", 1);
    oled_text(0, 28, "STM32F103", 1);
    oled_text(0, 40, "OLED OK  3C", 1);
    oled_flush();

    s_last_ms = 0;
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
