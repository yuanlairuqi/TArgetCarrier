/**
 * @file    oled.h
 * @brief   128x64 OLED (SSD1306 / SH1106) 驱动 —— 硬件无关层
 *
 *  设计目的:
 *    同一份驱动代码, 分别跑在 STM32F103(软件I2C) 和 ESP32-S3(软件I2C) 上,
 *    两块屏显示各自板子的实时状态。硬件相关的部分(拉引脚/延时)通过
 *    oled_hal_t 的函数指针注入, 驱动本身不碰任何寄存器。
 *
 *  接线 (按项目约定):
 *    ┌──────────────────────────────────────────────────┐
 *    │  OLED 引脚    第一块 → 野火 STM32F103C8T6         │
 *    │  ────────────────────────────────────────────    │
 *    │  GND          GND                                │
 *    │  VCC          3V3      ← 只能 3.3V, 不要接 5V     │
 *    │  SCL          PB6      (I2C1_SCL 默认脚)          │
 *    │  SDA          PB7      (I2C1_SDA 默认脚)          │
 *    ├──────────────────────────────────────────────────┤
 *    │  OLED 引脚    第二块 → M5Stamp-S3Bat DIP          │
 *    │  ────────────────────────────────────────────    │
 *    │  GND          GND                                │
 *    │  VCC          3V3                                │
 *    │  SCL          G9 / GPIO9                         │
 *    │  SDA          G8 / GPIO8                         │
 *    └──────────────────────────────────────────────────┘
 *
 *  ★ 为什么两块屏各用各的 I2C 引脚而不是挂在同一条总线上:
 *    两块板是电气隔离的(只共地), 各自供电。挂同一条总线会引入电平/地环路问题。
 *    I2C 是总线制, 同一条总线上地址冲突才有问题; 分开挂则地址相同也没关系。
 *
 *  ★ 为什么用软件 I2C 而不是硬件 I2C:
 *    1. 两块屏的刷新率要求极低(每秒几次), 软件 I2C 的 ~100kHz 完全够
 *    2. 不需要占用/配置硬件 I2C 外设, 不依赖框架的 Wire 库
 *    3. STM32F103 的硬件 I2C 有著名的勘误和死锁问题, 软件 I2C 更省心
 *    4. 同一份代码能在两颗完全不同的芯片上跑, 只换 HAL 实现
 */

#ifndef OLED_H
#define OLED_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 硬件抽象层 —— 由各平台实现                                          */
/* ------------------------------------------------------------------ */

typedef struct {
    /** 把 SCL 拉高/拉低 (level: 1=高, 0=低) */
    void (*set_scl)(uint8_t level);
    /** 把 SDA 拉高/拉低 */
    void (*set_sda)(uint8_t level);
    /** 读 SDA 电平 (用于 ACK 检测; 返回 1=高, 0=低) */
    uint8_t (*get_sda)(void);
    /** 微秒级延时 */
    void (*delay_us)(uint32_t us);
} oled_hal_t;

/* ------------------------------------------------------------------ */
/* 屏幕参数                                                           */
/* ------------------------------------------------------------------ */

#define OLED_WIDTH        128
#define OLED_HEIGHT       64
#define OLED_PAGES        (OLED_HEIGHT / 8)          /* 8 页 */
#define OLED_FB_SIZE      (OLED_WIDTH * OLED_PAGES)  /* 1024 字节 */

/* I2C 从机地址: SSD1306 常见 0x3C(7位), 有些模块是 0x3D */
#define OLED_I2C_ADDR_3C  0x3C
#define OLED_I2C_ADDR_3D  0x3D

typedef enum {
    OLED_CTRL_SSD1306 = 0,   /* 最常见, 128x64 */
    OLED_CTRL_SH1106  = 1,   /* 132x64 控制器, 可见 128 列, 需要列偏移 2 */
} oled_ctrl_t;

/* ------------------------------------------------------------------ */
/* API                                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief 初始化屏幕
 * @param hal      硬件抽象 (必须一直有效, 不能是临时变量)
 * @param ctrl     控制器型号
 * @param addr     7 位 I2C 地址 (OLED_I2C_ADDR_3C 或 0x3D)
 * @return true = 找到了屏幕并完成初始化
 */
bool oled_init(const oled_hal_t *hal, oled_ctrl_t ctrl, uint8_t addr);

/** 全屏清空 (只清显存, 需要调用 oled_flush 才会上屏) */
void oled_clear(void);

/** 把显存刷到屏幕 (整屏 1024 字节) */
void oled_flush(void);

/** 画一个像素 */
void oled_pixel(int16_t x, int16_t y, bool on);

/**
 * @brief 写一个 3x5 小字库里的字符
 * @param x 左上角像素坐标
 * @param y 左上角像素坐标
 * @param scale 放大倍数 (1 = 3x5, 2 = 6x10, 3 = 9x15, 4 = 12x20)
 */
void oled_char_3x5(int16_t x, int16_t y, char c, uint8_t scale);

/** 写字符串 (3x5 字库, 不换行, 超出屏幕自动截断) */
void oled_text(int16_t x, int16_t y, const char *s, uint8_t scale);

/** 画水平线 */
void oled_hline(int16_t x, int16_t y, int16_t w);

/** 画矩形框 (只画边框) */
void oled_rect(int16_t x, int16_t y, int16_t w, int16_t h);

/**
 * @brief 高级: 直接操作显存缓冲 (1024 字节, 按 page 排列)
 *        布局: buf[page*128 + x], 每字节纵向 8 像素, bit0 在上
 */
uint8_t *oled_framebuffer(void);

#ifdef __cplusplus
}
#endif

#endif /* OLED_H */
