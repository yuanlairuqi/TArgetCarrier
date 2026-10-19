/**
 * @file    oled.c
 * @brief   128x64 OLED (SSD1306/SH1106) 驱动实现 —— 软件 I2C + 3x5 字库
 *
 *  软件 I2C 时序 (标准模式, ~100kHz):
 *    起始: SCL 高时 SDA 由高变低
 *    停止: SCL 高时 SDA 由低变高
 *    数据: SCL 低时改 SDA, SCL 高时采样
 *    ACK : 发完 8 位后释放 SDA, 第 9 个时钟看 SDA 是否被从机拉低
 */

#include "oled.h"
#include <string.h>

#define OLED_HAL_CHECK()  do { if (s_hal == NULL) return; } while (0)

static const oled_hal_t *s_hal  = NULL;
static oled_ctrl_t       s_ctrl = OLED_CTRL_SSD1306;
static uint8_t           s_addr = OLED_I2C_ADDR_3C;
static uint8_t           s_fb[OLED_FB_SIZE];

/* ================================================================== */
/* 软件 I2C 底层                                                      */
/* ================================================================== */

static inline void scl_hi(void) { s_hal->set_scl(1); }
static inline void scl_lo(void) { s_hal->set_scl(0); }
static inline void sda_hi(void) { s_hal->set_sda(1); }
static inline void sda_lo(void) { s_hal->set_sda(0); }
static inline uint8_t sda_rd(void) { return s_hal->get_sda(); }

/* 软件 I2C 的半周期延时。100kHz -> 半个周期约 5us。
 * 用 2us 让总线跑得快一些(~250kHz), SSD1306 支持 400kHz, 安全。 */
#define I2C_HALF_DELAY_US   2u

static void i2c_delay(void)
{
    s_hal->delay_us(I2C_HALF_DELAY_US);
}

static void i2c_start(void)
{
    sda_hi(); i2c_delay();
    scl_hi(); i2c_delay();
    sda_lo(); i2c_delay();
    scl_lo(); i2c_delay();
}

static void i2c_stop(void)
{
    sda_lo(); i2c_delay();
    scl_hi(); i2c_delay();
    sda_hi(); i2c_delay();
}

/** 写一个字节, 返回 true = 从机给了 ACK */
static bool i2c_write_byte(uint8_t b)
{
    for (uint8_t i = 0; i < 8; i++) {
        if (b & 0x80u) {
            sda_hi();
        } else {
            sda_lo();
        }
        b <<= 1;
        i2c_delay();
        scl_hi(); i2c_delay();
        scl_lo(); i2c_delay();
    }

    /* 第 9 个时钟: 释放 SDA 读 ACK */
    sda_hi(); i2c_delay();
    scl_hi(); i2c_delay();
    bool ack = (sda_rd() == 0);
    scl_lo(); i2c_delay();
    return ack;
}

/** 发一串 "控制字节 + 数据", 用于 SSD1306 的 0x00(命令) / 0x40(数据) 前缀 */
static bool oled_send(uint8_t prefix, const uint8_t *data, uint16_t len)
{
    if (s_hal == NULL) {
        return false;
    }

    i2c_start();
    /* 从机地址 + 写位(0) */
    if (!i2c_write_byte((uint8_t)(s_addr << 1))) {
        i2c_stop();
        return false;
    }
    if (!i2c_write_byte(prefix)) {
        i2c_stop();
        return false;
    }
    for (uint16_t i = 0; i < len; i++) {
        if (!i2c_write_byte(data[i])) {
            i2c_stop();
            return false;
        }
    }
    i2c_stop();
    return true;
}

static void oled_cmd(uint8_t c)
{
    (void)oled_send(0x00, &c, 1);
}

/* ================================================================== */
/* 初始化                                                             */
/* ================================================================== */

bool oled_init(const oled_hal_t *hal, oled_ctrl_t ctrl, uint8_t addr)
{
    if (hal == NULL) {
        return false;
    }
    s_hal  = hal;
    s_ctrl = ctrl;
    s_addr = addr;

    /* 先给总线一点上电稳定时间 (等屏幕内部复位) */
    s_hal->delay_us(100000);

    /* 探测: 发地址, 看有没有 ACK。没 ACK 说明没接好/地址不对/没供电 */
    i2c_start();
    bool present = i2c_write_byte((uint8_t)(s_addr << 1));
    i2c_stop();
    if (!present) {
        return false;
    }

    /* ---- SSD1306 / SH1106 通用初始化序列 ---- */
    static const uint8_t init_seq[] = {
        0xAE,        /* 关显示 */
        0xD5, 0x80,  /* 时钟分频 */
        0xA8, 0x3F,  /* 复用率 64 */
        0xD3, 0x00,  /* 显示偏移 0 */
        0x40,        /* 起始行 0 */
        0x8D, 0x14,  /* 充电泵开 (3.3V 供电必须开) */
        0x20, 0x00,  /* 内存寻址模式 = 水平 */
        0xA1,        /* 段重映射 (左右方向) */
        0xC8,        /* COM 扫描方向反 (上下方向) */
        0xDA, 0x12,  /* COM 引脚配置 */
        0x81, 0xCF,  /* 对比度 */
        0xD9, 0xF1,  /* 预充电周期 */
        0xDB, 0x40,  /* VCOMH */
        0xA4,        /* 显示显存内容 */
        0xA6,        /* 正常显示(非反白) */
        0x2E,        /* 关滚动 */
        0xAF,        /* 开显示 */
    };
    for (uint16_t i = 0; i < sizeof(init_seq); i++) {
        oled_cmd(init_seq[i]);
    }

    oled_clear();
    oled_flush();
    return true;
}

/* ================================================================== */
/* 刷屏                                                               */
/* ================================================================== */

void oled_flush(void)
{
    if (s_hal == NULL) {
        return;
    }

    if (s_ctrl == OLED_CTRL_SH1106) {
        /* SH1106 是 132x64 控制器, 可见 128 列, 从第 2 列开始。
         * 它不支持 "水平寻址模式", 必须按页单独设地址。 */
        for (uint8_t page = 0; page < OLED_PAGES; page++) {
            oled_cmd((uint8_t)(0xB0 | page));          /* 页地址 */
            oled_cmd((uint8_t)(0x00 | (2 & 0x0F)));    /* 低列地址 +2 */
            oled_cmd((uint8_t)(0x10 | (2 >> 4)));      /* 高列地址 */
            (void)oled_send(0x40, &s_fb[page * OLED_WIDTH], OLED_WIDTH);
        }
    } else {
        /* SSD1306 水平寻址模式: 设一次起始地址, 之后连续写 */
        oled_cmd(0x21); oled_cmd(0);   oled_cmd(OLED_WIDTH - 1);  /* 列范围 */
        oled_cmd(0x22); oled_cmd(0);   oled_cmd(OLED_PAGES - 1);  /* 页范围 */
        (void)oled_send(0x40, s_fb, OLED_FB_SIZE);
    }
}

/* ================================================================== */
/* 绘图                                                               */
/* ================================================================== */

void oled_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

void oled_pixel(int16_t x, int16_t y, bool on)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    uint16_t idx = (uint16_t)((y / 8) * OLED_WIDTH + x);
    uint8_t  bit = (uint8_t)(1u << (y & 7));
    if (on) {
        s_fb[idx] |= bit;
    } else {
        s_fb[idx] &= (uint8_t)~bit;
    }
}

void oled_hline(int16_t x, int16_t y, int16_t w)
{
    for (int16_t i = 0; i < w; i++) {
        oled_pixel(x + i, y, true);
    }
}

void oled_rect(int16_t x, int16_t y, int16_t w, int16_t h)
{
    oled_hline(x, y, w);
    oled_hline(x, (int16_t)(y + h - 1), w);
    for (int16_t i = 0; i < h; i++) {
        oled_pixel(x, y + i, true);
        oled_pixel((int16_t)(x + w - 1), y + i, true);
    }
}

uint8_t *oled_framebuffer(void)
{
    return s_fb;
}

/* ================================================================== */
/* 3x5 小字库                                                         */
/* ================================================================== */
/*
 * 每个字符 3 列, 每列 5 位 (bit4 在最上, bit0 在最下)。
 * 覆盖: 数字 0-9, 负号, 小数点, 冒号, 斜杠, 百分号,
 *       大写字母 A~Z (只做常用、笔画少的部分), 小写统一按大写显示。
 * 未知字符显示为空格 —— 屏上不会出现乱码。
 */
typedef struct {
    char    ch;
    uint8_t col[3];
} glyph3x5_t;

static const glyph3x5_t s_font[] = {
    {' ', {0x00, 0x00, 0x00}},
    {'-', {0x00, 0x04, 0x00}},
    {'.', {0x00, 0x00, 0x20}},
    {':', {0x00, 0x0A, 0x00}},
    {'/', {0x10, 0x04, 0x00}},
    {'%', {0x12, 0x04, 0x09}},
    {'+', {0x04, 0x0E, 0x04}},
    {'_', {0x10, 0x10, 0x10}},

    {'0', {0x1F, 0x11, 0x1F}},
    {'1', {0x00, 0x1F, 0x00}},
    {'2', {0x1D, 0x15, 0x17}},
    {'3', {0x15, 0x15, 0x1F}},
    {'4', {0x07, 0x04, 0x1F}},
    {'5', {0x17, 0x15, 0x1D}},
    {'6', {0x1F, 0x15, 0x1D}},
    {'7', {0x01, 0x19, 0x05}},
    {'8', {0x1F, 0x15, 0x1F}},
    {'9', {0x17, 0x15, 0x1F}},

    {'A', {0x1E, 0x05, 0x1E}},
    {'B', {0x1F, 0x15, 0x0A}},
    {'C', {0x0E, 0x11, 0x11}},
    {'D', {0x1F, 0x11, 0x0E}},
    {'E', {0x1F, 0x15, 0x15}},
    {'F', {0x1F, 0x05, 0x05}},
    {'G', {0x0E, 0x11, 0x1D}},
    {'H', {0x1F, 0x04, 0x1F}},
    {'I', {0x11, 0x1F, 0x11}},
    {'J', {0x08, 0x10, 0x0F}},
    {'K', {0x1F, 0x04, 0x1B}},
    {'L', {0x1F, 0x10, 0x10}},
    {'M', {0x1F, 0x02, 0x1F}},
    {'N', {0x1F, 0x0E, 0x1F}},
    {'O', {0x0E, 0x11, 0x0E}},
    {'P', {0x1F, 0x05, 0x02}},
    {'Q', {0x0E, 0x19, 0x1E}},
    {'R', {0x1F, 0x05, 0x1A}},
    {'S', {0x12, 0x15, 0x09}},
    {'T', {0x01, 0x1F, 0x01}},
    {'U', {0x0F, 0x10, 0x0F}},
    {'V', {0x07, 0x18, 0x07}},
    {'W', {0x1F, 0x08, 0x1F}},
    {'X', {0x1B, 0x04, 0x1B}},
    {'Y', {0x03, 0x1C, 0x03}},
    {'Z', {0x19, 0x15, 0x13}},
};

static const uint8_t s_glyph_space[3] = {0x00, 0x00, 0x00};

static const uint8_t *glyph_lookup(char c)
{
    if (c >= 'a' && c <= 'z') {
        c = (char)(c - 'a' + 'A');
    }
    for (uint16_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == c) {
            return s_font[i].col;
        }
    }
    return s_glyph_space;
}

void oled_char_3x5(int16_t x, int16_t y, char c, uint8_t scale)
{
    if (scale == 0) {
        scale = 1;
    }
    const uint8_t *g = glyph_lookup(c);

    for (uint8_t col = 0; col < 3; col++) {
        uint8_t bits = g[col];
        for (uint8_t row = 0; row < 5; row++) {
            if (bits & (uint8_t)(1u << row)) {
                /* 每个字模像素放大成 scale x scale 的方块 */
                for (uint8_t dy = 0; dy < scale; dy++) {
                    for (uint8_t dx = 0; dx < scale; dx++) {
                        oled_pixel((int16_t)(x + col * scale + dx),
                                   (int16_t)(y + (4 - row) * scale + dy), true);
                    }
                }
            }
        }
    }
}

void oled_text(int16_t x, int16_t y, const char *s, uint8_t scale)
{
    if (s == NULL) {
        return;
    }
    if (scale == 0) {
        scale = 1;
    }

    int16_t cx = x;
    while (*s != '\0') {
        /* 3 列字宽 + 1 列间隔, 再乘放大倍数 */
        if (cx + 3 * scale > OLED_WIDTH) {
            break;
        }
        oled_char_3x5(cx, y, *s, scale);
        cx = (int16_t)(cx + 4 * scale);
        s++;
    }
}
