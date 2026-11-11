/**
 * @file    timer_drv.h
 * @brief   1kHz 控制节拍定时器 (C 语言接口)
 *
 *  实现放在 timer_drv.cpp 里, 因为 STM32duino 的 HardwareTimer 是 C++ 类。
 *  这样 app_main.c 仍然可以是纯 C —— 以后要挪到 CubeMX/Keil 工程时,
 *  只要换掉这一个包装层即可。
 */

#ifndef TIMER_DRV_H
#define TIMER_DRV_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动 1kHz 周期中断
 * @param callback 每个节拍调用一次的回调 (在中断上下文执行, 必须短)
 */
void timer_drv_start_1khz(void (*callback)(void));

/**
 * @brief 启动 FreeRTOS 的 1kHz 节拍源 (TIM4, 见 rtos_start.c 说明)
 *
 * 由 FreeRTOS 端口在 xPortStartScheduler() 里通过覆盖
 * vPortSetupTimerInterrupt() 调用一次; 不要在业务代码里重复调用。
 * 中断里只调用 xPortSysTickHandler(), 不做其它事情。
 */
void timer_drv_start_rtos_tick(void);

/** 停掉节拍 (调试用) */
void timer_drv_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* TIMER_DRV_H */
