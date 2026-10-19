/**
 * @file    display.h
 * @brief   桥接端 OLED 状态显示 —— 接口
 *
 *  与 oled.c (纯驱动) 分开: 这个文件负责"画什么内容",
 *  oled.c 只负责"怎么把像素送到屏上"。
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化屏幕并显示开机画面
 * @return true = 屏幕存在且初始化成功; false = 没接屏幕, 后续自动跳过
 */
bool display_init(void);

/** 周期调用刷新状态 (内部自带 100ms 限速) */
void display_tick(uint32_t now_ms);

/** 屏幕是否可用 */
bool display_available(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
