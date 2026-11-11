/**
 * @file    FreeRTOSConfig.h
 * @brief   靶车 STM32F103C8T6 —— FreeRTOS-Kernel V11.2.0 配置
 *
 *  设计要点 (与 docs/通信可靠性与FreeRTOS移植说明.md 的任务表对应):
 *    1. 只做静态分配 (configSUPPORT_DYNAMIC_ALLOCATION=0):
 *       20KB RAM 的 F103 上不引入 heap_4, 任务栈/队列/互斥量全部编译期分配,
 *       分配失败在编译/链接期就能暴露, 运行期没有堆碎片, 也就不用 malloc 失败钩子。
 *    2. 1 kHz 系统节拍: TIM2 另行提供 1 kHz 控制节拍通知 (见 timer_drv.cpp),
 *       FreeRTOS 自己的 tick 只用来做延时/超时, 不承担控制节拍精度。
 *    3. SysTick 归属: STM32duino/HAL 的 SysTick_Handler 继续只负责 HAL_IncTick()/
 *       millis(); FreeRTOS **不使用 SysTick**, 而是用 TIM4 (HardwareTimer 注册回调,
 *       向量仍由核心持有) 产生自己的 1 kHz 节拍。原因: 核心的 SysTick_Handler 是
 *       框架内强符号, 无法安全地再映射一次; 而 TIM4 的用法与本工程已有的 TIM2
 *       控制节拍完全同源, 不依赖核心内部是否调用 HAL_SYSTICK_IRQHandler。
 *       两者都是 1 kHz, 但彼此独立: HAL 时基给 delay()/millis(), RTOS 时基给
 *       任务调度, 不会出现"双节拍/重复向量"。
 *    4. 中断优先级: Cortex-M3 4 位优先级。只有优先级数值 >=5 (即逻辑上不高于
 *       configMAX_SYSCALL_INTERRUPT_PRIORITY) 的中断才允许调用 FreeRTOS 的
 *       FromISR API。本工程的 TIM4(RTOS tick)=5、TIM2(控制节拍)=5 使用 FromISR;
 *       USART1/CAN 都在任务里轮询, 不产生 "ISR 里调错 API" 的风险。
 *    5. 栈溢出检测 = 2 (栈末尾哨兵 + 水位), 并提供钩子做失能 + 复位。
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ---------------- 调度器 ---------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 1
#define configUSE_TICKLESS_IDLE                 0
#define configCPU_CLOCK_HZ                      72000000UL
#define configTICK_RATE_HZ                      1000
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configMAX_PRIORITIES                    6
#define configMINIMAL_STACK_SIZE                128      /* words, idle 任务 512 B */
#define configMAX_TASK_NAME_LEN                 10
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0

/* ---------------- 同步原语 ---------------- */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           0
#define configUSE_QUEUE_SETS                    0
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configQUEUE_REGISTRY_SIZE               0

/* ---------------- 内存: 只用静态分配 ---------------- */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        0
#define configTOTAL_HEAP_SIZE                   0
#define configUSE_MALLOC_FAILED_HOOK            0
#define configKERNEL_PROVIDED_STATIC_MEMORY     0

/* ---------------- 可选功能 ---------------- */
#define configUSE_TIMERS                        0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_CO_ROUTINES                   0
#define configUSE_APPLICATION_TASK_TAG          0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configRECORD_STACK_HIGH_ADDRESS         0

/* ---------------- API 可见性 ---------------- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     0
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_uxTaskGetStackHighWaterMark2    0
#define INCLUDE_xTaskGetIdleTaskHandle          0
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xQueueGetMutexHolder            0
#define INCLUDE_xSemaphoreGetMutexHolder        0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xTimerPendFunctionCall          0

/* ---------------- 中断优先级 (Cortex-M3, 4 bit) ---------------- */
#define configPRIO_BITS                             4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY     15
/* 5 = 允许调用 FreeRTOS API 的最低逻辑优先级 (数值越大优先级越低) */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* ---------------- 向量映射 ----------------
 * 只映射 SVC 和 PendSV。
 * ★ 故意不映射 xPortSysTickHandler -> SysTick_Handler:
 *   STM32duino 核心已定义 SysTick_Handler (HAL_IncTick/millis), 重复映射会
 *   造成双节拍或重复定义。FreeRTOS 的 1 kHz 节拍改由 TIM4 产生:
 *   见 src/timer_drv.cpp 的 timer_drv_start_rtos_tick() 与 src/rtos_start.c
 *   里强符号覆盖的 vPortSetupTimerInterrupt()。*/
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler

/* 本工程不使用 SysTick 作为 RTOS 节拍, 下面这些仅用于满足端口编译。
 * configUSE_TICKLESS_IDLE=0, 端口不会去改 SysTick 的重装载值。 */
#define configSYSTICK_CLOCK_HZ  configCPU_CLOCK_HZ

/* ---------------- 断言 ----------------
 * 断言失败 = 调度器/内存/优先级配置出现严重错误: 先尽力失能电机再复位,
 * 不留在原地继续跑 (运动控制最怕"卡住但电机还在动")。 */
void app_rtos_assert_failed(const char *file, unsigned long line);
#define configASSERT(x) \
    do { if ((x) == 0) { app_rtos_assert_failed(__FILE__, __LINE__); } } while (0)

/* 栈溢出钩子: 见 src/rtos_start.c (失能电机 + 复位) */

#endif /* FREERTOS_CONFIG_H */
