/**
 * @file    oled_hal_stm32.c
 * @brief   STM32F103 的 OLED 硬件抽象层 (软件 I2C, PB6=SCL / PB7=SDA)
 *
 *  ★ 为什么用软件 I2C 而不是硬件 I2C1:
 *    1. 屏幕每秒只刷几次, 软件 I2C 的 ~250kHz 绰绰有余
 *    2. 不占用/初始化 I2C1 外设, 避免和框架或后续外设抢资源
 *    3. F103 的硬件 I2C 有著名的勘误(总线锁死), 软件 I2C 更稳
 *    4. 同一份 oled.c 通过函数指针就能跑在 STM32 和 ESP32 上
 *
 *  引脚说明:
 *    PB6 = I2C1_SCL (F103 的 I2C1 默认脚之一)
 *    PB7 = I2C1_SDA
 *    这两根脚在本工程里没有被 USART1(PA9/PA10)、CAN1(PB8/PB9, 重映射)、
 *    SWD(PA13/PA14) 占用, 所以安全。
 *
 *  ★ 与 USB 无关: 本板的 PA11/PA12 焊到 USB 座, 我们一个都不碰。
 */

#include "display.h"
#include "oled.h"

#include "stm32f1xx.h"

/* ------------------------------------------------------------------ */
/* GPIO 配置: PB6 / PB7 开漏输出 (I2C 标准做法)                        */
/* ------------------------------------------------------------------ */
/* F103 的 GPIO 配置规则 (RM0008):
 *   开漏输出: CNF = 01, MODE = 11 (50MHz)  -> 0x7
 *   CRL 管 PB0..PB7, 每脚 4 位。
 *   PB6 -> CRL bit27:24,  PB7 -> CRL bit31:28
 * 开漏的好处: 输出 1 时是"释放"(由外部上拉拉高), 输出 0 是拉低。
 * 即使模块自带 4.7k 上拉也能正常工作; 若模块没上拉, 需要外部加 4.7k 到 3V3。 */

static void oled_gpio_init(void)
{
    RCC->APB2ENR |= RCC_APB2ENR_IOPBEN;

    uint32_t crl = GPIOB->CRL;
    crl &= ~(0xFu << 24);          /* 清 PB6 */
    crl |=  (0x7u << 24);          /* PB6: 开漏输出 50MHz */
    crl &= ~(0xFu << 28);          /* 清 PB7 */
    crl |=  (0x7u << 28);          /* PB7: 开漏输出 50MHz */
    GPIOB->CRL = crl;

    /* 初始都释放(高电平) */
    GPIOB->BSRR = (1u << 6) | (1u << 7);
}

/* ------------------------------------------------------------------ */
/* HAL 实现                                                           */
/* ------------------------------------------------------------------ */

static void hal_set_scl(uint8_t level)
{
    if (level) {
        GPIOB->BSRR = (1u << 6);            /* 高: 置位 */
    } else {
        GPIOB->BRR  = (1u << 6);            /* 低: 复位 */
    }
}

static void hal_set_sda(uint8_t level)
{
    if (level) {
        GPIOB->BSRR = (1u << 7);
    } else {
        GPIOB->BRR  = (1u << 7);
    }
}

static uint8_t hal_get_sda(void)
{
    /* 开漏输出模式下 IDR 仍能读到引脚真实电平, 所以可以直接读 */
    return (GPIOB->IDR & (1u << 7)) ? 1u : 0u;
}

/* 微秒级忙等。72MHz 下每次循环约 4~5 个周期(volatile 强制读写内存),
 * 取 14 次约等于 1us。精度不要求很高, I2C 时序有 2x 余量。
 * 不用 DWT 是为了不依赖核心是否使能了 DWT 周期计数器。 */
static void hal_delay_us(uint32_t us)
{
    while (us-- > 0u) {
        for (volatile uint32_t i = 0; i < 14u; i++) {
            __NOP();
        }
    }
}

/* HAL 实例必须是静态的: oled_init 只保存指针, 不能传临时变量 */
static const oled_hal_t s_hal = {
    .set_scl  = hal_set_scl,
    .set_sda  = hal_set_sda,
    .get_sda  = hal_get_sda,
    .delay_us = hal_delay_us,
};

const oled_hal_t *oled_hal_stm32(void)
{
    return &s_hal;
}

void oled_hal_stm32_gpio_init(void)
{
    oled_gpio_init();
}
