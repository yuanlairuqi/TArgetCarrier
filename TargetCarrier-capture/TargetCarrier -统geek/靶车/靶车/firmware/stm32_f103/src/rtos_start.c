/**
 * @file    rtos_start.c
 * @brief   FreeRTOS 与 STM32duino/HAL 的粘合层 + 致命错误处理
 *
 *  为什么需要这一层 (对应移植要求 6/7/9):
 *    1. SysTick 归属:
 *       STM32duino 核心已经定义了 SysTick_Handler, 它负责 HAL_IncTick()/millis()。
 *       本工程不在 FreeRTOSConfig.h 里把 xPortSysTickHandler 映射成
 *       SysTick_Handler (那会重复定义/双节拍), 而是强符号覆盖
 *       vPortSetupTimerInterrupt(), 用 TIM4 产生独立的 1 kHz RTOS 节拍。
 *       两个时基都是 1 kHz, 但互不依赖。
 *    2. 静态分配的 Idle 任务内存: 本工程关闭动态分配, 需要由应用提供。
 *    3. 栈溢出/断言: 先尽力发失能帧, 再复位; 绝不停在"卡住但电机还在按旧目标出力"
 *       的状态。
 */

#include "FreeRTOS.h"
#include "task.h"

#include "stm32f1xx.h"

#include "app_main.h"

/* ------------------------------------------------------------------ */
/* 静态 Idle 任务                                                     */
/* ------------------------------------------------------------------ */

static StaticTask_t s_idle_tcb;
static StackType_t  s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   configSTACK_DEPTH_TYPE *puxIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *puxIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* ------------------------------------------------------------------ */
/* FreeRTOS 节拍源: TIM4 (不是 SysTick)                               */
/* ------------------------------------------------------------------ */
/* FreeRTOS Cortex-M3 端口默认在 xPortStartScheduler() 里调用
 * vPortSetupTimerInterrupt() 配置 SysTick。但本工程的 SysTick 已经由
 * STM32duino 核心用来驱动 HAL_IncTick()/millis(), 而核心的 SysTick_Handler
 * 是框架内强符号, 不能安全地再映射给 RTOS。
 *
 * 解决: 用强符号覆盖弱定义的 vPortSetupTimerInterrupt(), 让它去启动 TIM4 的
 * 1 kHz 节拍 (HardwareTimer 注册回调, 向量由核心持有, 与 TIM2 控制节拍同源),
 * 回调里调用 xPortSysTickHandler()。这样:
 *   - SysTick 仍只属于 HAL (millis/delay 正常);
 *   - FreeRTOS 的 1 kHz tick 独立、精确、可单步验证;
 *   - 不产生重复向量, 不改框架源码。
 *
 * 注意: 本函数在 xPortStartScheduler() 内、全局中断关闭的状态下被调用,
 * 只能做"配置定时器 + 使能中断", 不能等待/阻塞。 */
void timer_drv_start_rtos_tick(void);

void vPortSetupTimerInterrupt(void)
{
    timer_drv_start_rtos_tick();
}

/* ------------------------------------------------------------------ */
/* 致命错误: 失能 + 复位                                              */
/* ------------------------------------------------------------------ */

static void app_rtos_panic(void)
{
    /* 1. 尽力把失能命令写进 CAN 邮箱 (不取锁、不等待) */
    app_emergency_disable_motor();

    /* 2. 复位 MCU。复位后: CAN 重新初始化、ARM 状态清零、电机默认失能。
     *    这比"卡在错误处理循环里, 电机还按上一帧目标出力"安全。 */
    NVIC_SystemReset();
    for (;;) {
        /* NVIC_SystemReset 正常不返回 */
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    app_rtos_panic();
}

void app_rtos_assert_failed(const char *file, unsigned long line)
{
    (void)file;
    (void)line;
    app_rtos_panic();
}
