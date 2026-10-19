/**
 * @file    oled_hal_esp32.h
 * @brief   M5Stamp-S3Bat DIP / ESP32-S3 的 OLED 硬件抽象层声明
 *
 *  接线 (按项目约定):
 *    OLED GND -> GND
 *    OLED VCC -> 3V3   (★ 只能 3.3V)
 *    OLED SCL -> G9 / GPIO9
 *    OLED SDA -> G8 / GPIO8
 *
 *  ★ 为什么选 G8/G9:
 *    本工程桥接端的串口链路占了 G1/G2, 这两个不能动。
 *    M5Stamp-S3Bat DIP 的排针可用脚里, G8/G9 是空闲的普通 GPIO,
 *    不和板载电源管理芯片(M5PM1 占用 G13/G15 的 I2C)、
 *    也不和 UART0(G43/G44) 冲突, 所以最适合做软件 I2C。
 *
 *  ★ 与 STM32 那块屏是两条独立总线:
 *    两块板电气隔离, 各自供电, 只共地。I2C 是总线制, 分开放即使
 *    地址都是 0x3C 也互不影响。
 */

#ifndef OLED_HAL_ESP32_H
#define OLED_HAL_ESP32_H

#include "oled.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 返回 ESP32 的 HAL 实例 (静态存储, 可以长期保存指针) */
const oled_hal_t *oled_hal_esp32(void);

/** 把 G8/G9 配置成开漏输出 + 内部上拉 */
void oled_hal_esp32_gpio_init(void);

#ifdef __cplusplus
}
#endif

#endif /* OLED_HAL_ESP32_H */
