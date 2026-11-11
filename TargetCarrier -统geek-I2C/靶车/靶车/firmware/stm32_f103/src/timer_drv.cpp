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

    /* ★ 补一刀: 某些核心版本里 attachInterrupt 没有真正打开"更新中断 + NVIC",
     *   表现就是定时器在跑、但回调一次都不进 (s_ctrl_ticks 永远是 0)。
     *   这里显式再确认一遍; 都是幂等操作, 重复做没有副作用。 */
    TIM2->DIER |= TIM_DIER_UIE;
    /* FreeRTOS 移植后: 中断里要调用 xTaskNotifyFromISR(), 所以优先级必须
     * 不高于 configMAX_SYSCALL_INTERRUPT_PRIORITY (逻辑优先级数值 >= 5)。
     * 5 = 允许 FromISR, 又高于所有任务 (任务优先级 1..5) 的调度优先级。 */
    HAL_NVIC_SetPriority(TIM2_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM2_IRQn);
    TIM2->EGR = TIM_EGR_UG;          /* 重新装载 PSC/ARR, 立即从 0 开始计数 */
}

extern "C" void timer_drv_stop(void)
{
    if (s_timer != nullptr) {
        s_timer->pause();
    }
}

/* ================================================================== */
/* FreeRTOS 1kHz 节拍源 (TIM4)                                        */
/* ================================================================== */
/* 为什么用 TIM4 而不是 SysTick:
 *   STM32duino 核心已经拥有 SysTick_Handler (HAL_IncTick/millis), 且是框架内
 *   强符号。若把 FreeRTOS 的 xPortSysTickHandler 映射成 SysTick_Handler,
 *   要么重复定义, 要么和核心抢同一个向量。改用 TIM4:
 *     - 向量仍由核心的 HardwareTimer.cpp 持有 (我们不重复定义 IRQHandler);
 *     - 与本工程 TIM2 控制节拍完全同源, 行为可预期;
 *     - SysTick 保持只服务 HAL 时基, 两个 1kHz 时钟互不干扰。
 * 中断优先级 5 = 允许调用 FreeRTOS FromISR 的最高逻辑优先级 (数值>=5)。 */
extern "C" void xPortSysTickHandler(void);

static HardwareTimer *s_rtos_timer = nullptr;

static void timer_rtos_tick_irq(void)
{
    /* 只做节拍递增 + 必要时触发 PendSV, 由 FreeRTOS 端口实现 */
    xPortSysTickHandler();
}

extern "C" void timer_drv_start_rtos_tick(void)
{
    if (s_rtos_timer == nullptr) {
        s_rtos_timer = new HardwareTimer(TIM4);
    }
    s_rtos_timer->pause();
    s_rtos_timer->setPrescaleFactor(72);                     /* 72MHz / 72 = 1MHz */
    s_rtos_timer->setOverflow(CTRL_TICK_HZ_LOCAL, HERTZ_FORMAT);  /* 1MHz / 1000 = 1kHz */
    s_rtos_timer->attachInterrupt(timer_rtos_tick_irq);

    /* 与 TIM2 一样显式确认更新中断和 NVIC (某些核心版本 attachInterrupt 不做) */
    TIM4->DIER |= TIM_DIER_UIE;
    HAL_NVIC_SetPriority(TIM4_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(TIM4_IRQn);

    s_rtos_timer->refresh();
    s_rtos_timer->resume();
}
