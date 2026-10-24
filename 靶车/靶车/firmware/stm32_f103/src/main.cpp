/**
 * @file    main.cpp
 * @brief   靶车 —— STM32F103C8T6 控制器入口 (Arduino/STM32duino 框架)
 *
 *  这里只做框架适配, 真正的逻辑全在 app_main.c (纯 C, 方便以后挪到
 *  CubeMX/HAL 工程或者 Keil 工程里复用)。
 *
 *  编译框架: PlatformIO + framework-arduinoststm32 (STM32duino)
 *    选它的原因: 工具链 (arm-none-eabi-gcc) 已经在本机, 编译/烧录一条命令,
 *    而且我们只用到它的启动文件和 HAL 时基, 外设全部裸寄存器操作,
 *    不受 Arduino 库封装的限制。
 */

#include <Arduino.h>

extern "C" {
#include "app_main.h"
#include "config.h"
#include "display.h"
}

void setup(void)
{
    /* ★ 先初始化 OLED, 让屏幕尽早亮起来显示状态。
     *   没接屏幕时 display_init() 返回 false, 后续刷新自动跳过, 不影响控制。 */
    display_init();

    app_setup();
}

void loop(void)
{
    app_loop();
}
