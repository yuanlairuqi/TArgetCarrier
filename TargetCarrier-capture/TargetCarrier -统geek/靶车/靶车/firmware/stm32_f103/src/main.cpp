/**
 * @file    main.cpp
 * @brief   靶车 —— STM32F103C8T6 控制器入口 (STM32duino + FreeRTOS)
 *
 *  本文件只做框架适配:
 *    - setup() 里的 app_setup() 会初始化外设、创建任务并启动 FreeRTOS 调度器
 *      (正常不返回);
 *    - loop() 在调度器启动后不会被执行, 保留空实现只是为了满足 Arduino 框架
 *      的链接要求。所有业务都在 app_main.c 的任务里。
 *
 *  ★ 不要在这里写 while(1)/delay 之类的逻辑:
 *    调度器启动后 Arduino 的 loop 任务本身也变成普通任务, 再放业务会破坏
 *    "控制任务最高优先级 + 单一 CAN 发送者"的设计。
 */

#include <Arduino.h>

extern "C" {
#include "app_main.h"
}

void setup(void)
{
    app_setup();   /* 初始化外设/任务/队列, 然后 vTaskStartScheduler() */
}

void loop(void)
{
    /* 调度器接管后不会返回这里; 若真的返回, 说明调度器启动失败,
     * app_setup() 内部已经做了失能 + 死循环处理。 */
}
