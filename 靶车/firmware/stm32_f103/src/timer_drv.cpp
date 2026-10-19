/**
 * @file    timer_drv.cpp
 * @brief   1kHz 控制节拍定时器 —— STM32duino HardwareTimer 包装
 *
 *  ★ 为什么不用裸 TIM2 寄存器 + 自己写 TIM2_IRQHandler:
 *    STM32duino 核心的 HardwareTimer.cpp 里已经定义了 TIM2_IRQHandler
 *    (以及 TIM1/3/4/5/9.. 的), 我们再定义一个就会链接失败:
 *      "multiple definition of `TIM2_IRQHandler'"
 *    所以这里用核心的 HardwareTimer 类注册回调, 中断向量由核心持有。
 *
 *  ★ 为什么要有这个 .cpp:
 *    HardwareTimer 是 C++ 类, 而 app_main.c 是纯 C。把 C++ 依赖关在这个
 *    文件里, 主逻辑就能保持可移植的纯 C。
 */

#include <Arduino.h>
#include "timer_drv.h"
#include "config.h"

/* 1kHz: 定时器时钟 72MHz, PSC 预分频到 1MHz, ARR = 1000 -> 1ms */
#define CTRL_TICK_HZ_LOCAL   1000u

static HardwareTimer *s_timer = nullptr;
static void (*s_callback)(void) = nullptr;

static void timer_irq_handler(void)
{
    if (s_callback != nullptr) {
        s_callback();
    }
}

extern "C" void timer_drv_start_1khz(void (*callback)(void))
{
    s_callback = callback;

    if (s_timer == nullptr) {
        s_timer = new HardwareTimer(TIM2);
    }

    s_timer->pause();
    s_timer->setPrescaleFactor(72);                  /* 72MHz / 72 = 1MHz */
    s_timer->setOverflow(CTRL_TICK_HZ_LOCAL, HERTZ_FORMAT);  /* 1MHz / 1000 = 1kHz */
    s_timer->attachInterrupt(timer_irq_handler);
    s_timer->refresh();
    s_timer->resume();
}

extern "C" void timer_drv_stop(void)
{
    if (s_timer != nullptr) {
        s_timer->pause();
    }
}
