/**
 * @file    display.h
 * @brief   各板 OLED 状态显示 —— 平台无关接口
 *
 *  与 oled.c (纯驱动) 分开: 这个文件负责"往上画什么内容",
 *  oled.c 只负责"怎么把像素送到屏上"。这样换屏幕型号/换布局
 *  不用动驱动。
 */

#ifndef DISPLAY_H
#define DISPLAY_H

#include <stdint.h>
#include <stdbool.h>
#include "oled.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化屏幕并做一次开机自检显示
 * @return true = 屏幕存在且初始化成功; false = 没接屏幕, 后续刷新自动跳过
 */
bool display_init(void);

/** 周期调用, 刷新状态 (内部自带限速, 可以每轮循环都调) */
void display_tick(uint32_t now_ms);

/** 屏幕是否可用 */
bool display_available(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_H */
