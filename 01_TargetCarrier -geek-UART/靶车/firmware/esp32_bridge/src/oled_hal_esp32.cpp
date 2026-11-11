/**
 * @file    oled_hal_esp32.cpp
 * @brief   ESP32-S3 的 OLED 硬件抽象层 (软件 I2C, G9=SCL / G8=SDA)
 *
 *  用 ESP-IDF 的寄存器级 GPIO 操作 (GPIO.out_w1ts / GPIO.in) 而不是
 *  Arduino 的 digitalWrite():
 *    软件 I2C 对时序敏感, 直接写寄存器省掉函数调用开销, 时序更可预测。
 *
 *  用 "开漏 + 内部上拉" 驱动:
 *    输出 1 -> 释放总线, 由上拉拉高
 *    输出 0 -> 拉低
 *  与 STM32 侧实现一致, 也符合 I2C 的线与电气规范。
 */

#include <Arduino.h>
#include <soc/gpio_struct.h>

#include "oled_hal_esp32.h"

/* ------------------------------------------------------------------ */
/* 引脚定义 —— 改这里就能换脚                                          */
/* ------------------------------------------------------------------ */
#define OLED_SCL_GPIO   9   /* G9  */
#define OLED_SDA_GPIO   8   /* G8  */

/* ------------------------------------------------------------------ */
/* GPIO 初始化                                                        */
/* ------------------------------------------------------------------ */

extern "C" void oled_hal_esp32_gpio_init(void)
{
    /* OUTPUT_OPEN_DRAIN: 输出 1 高阻、输出 0 拉低, 符合 I2C 空闲为高的要求 */
    pinMode(OLED_SCL_GPIO, OUTPUT_OPEN_DRAIN);
    pinMode(OLED_SDA_GPIO, OUTPUT_OPEN_DRAIN);

    /* 打开内部上拉 (模块通常也有 4.7k, 双保险; 和开漏输出不冲突) */
    gpio_set_pull_mode((gpio_num_t)OLED_SCL_GPIO, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)OLED_SDA_GPIO, GPIO_PULLUP_ONLY);

    /* 初始释放总线 */
    GPIO.out_w1ts = (1u << OLED_SCL_GPIO) | (1u << OLED_SDA_GPIO);
}

/* ------------------------------------------------------------------ */
/* HAL 实现                                                           */
/* ------------------------------------------------------------------ */

static void hal_set_scl(uint8_t level)
{
    if (level) {
        GPIO.out_w1ts = (1u << OLED_SCL_GPIO);   /* 置高 */
    } else {
        GPIO.out_w1tc = (1u << OLED_SCL_GPIO);   /* 拉低 */
    }
}

static void hal_set_sda(uint8_t level)
{
    if (level) {
        GPIO.out_w1ts = (1u << OLED_SDA_GPIO);
    } else {
        GPIO.out_w1tc = (1u << OLED_SDA_GPIO);
    }
}

static uint8_t hal_get_sda(void)
{
    /* 读输入寄存器, 开漏模式下能读到引脚真实电平 */
    return (GPIO.in & (1u << OLED_SDA_GPIO)) ? 1u : 0u;
}

static void hal_delay_us(uint32_t us)
{
    /* ESP32-S3 240MHz: delayMicroseconds 内部已按 CPU 频率标定, 比手写循环准 */
    delayMicroseconds(us);
}

/* 必须静态存储 —— oled_init 只保存指针 */
static const oled_hal_t s_hal = {
    .set_scl  = hal_set_scl,
    .set_sda  = hal_set_sda,
    .get_sda  = hal_get_sda,
    .delay_us = hal_delay_us,
};

extern "C" const oled_hal_t *oled_hal_esp32(void)
{
    return &s_hal;
}
