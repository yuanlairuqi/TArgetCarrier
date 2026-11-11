/**
 * @file    display_lcd.h
 * @brief   主机板 (Waveshare ESP32-S3-GEEK) 板载 1.14" ST7789 屏
 *
 *  这块屏是板子自带的 (240x135 IPS, 4 线 SPI, 控制器 ST7789), 之前主机固件
 *  完全没写显示代码, 所以屏幕一直是黑的 —— 不是硬件坏。
 *
 *  引脚 (来自 Waveshare 官方 demo / Zephyr 板级 dts):
 *      SCK=GPIO12  MOSI=GPIO11  CS=GPIO10  DC=GPIO8  RST=GPIO9  背光=GPIO7
 */

#ifndef DISPLAY_LCD_H
#define DISPLAY_LCD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化屏 (失败返回 false, 之后所有绘制自动跳过, 不影响控制逻辑) */
bool lcd_init(void);

/** 整屏填充一个 RGB565 颜色 (自检/诊断用) */
void lcd_fill(uint16_t color);

/** 整屏清成背景色 (上电时显存是随机值, 不清就是"有字但背景花") */
void lcd_clear(void);

/** 一次刷新多行 (10x14 字模, 每行 12 字符, 最多 12 行) */
void lcd_show_lines(const char *const *lines, int count);

/** 一次刷新 5 行文字 (兼容旧调用) */
void lcd_show(const char *l1, const char *l2, const char *l3,
              const char *l4, const char *l5);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_LCD_H */
