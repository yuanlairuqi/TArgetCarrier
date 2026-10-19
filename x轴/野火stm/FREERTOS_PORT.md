# STM32 FreeRTOS 移植

日期：2026-10-19。FreeRTOS-Kernel 固定 V11.2.0，GCC ARM_CM3 原生接口，无 CMSIS-RTOS 封装。内核源码和 MIT 许可证已随工程保存，来源及下载校验值见 Middlewares/Third_Party/FreeRTOS-Kernel/SOURCE.md。

## 任务与功能

- control：优先级 2，静态栈 512 words（2048 bytes），每 1 ms 唤醒。初始化 UART 与一次性运动，循环先 RobotUART_Poll，再 Motor_Process。保持 STOP 优先处理、UART 超时和 TIM1 硬件脉冲逻辑。
- rgb：仅闪灯启用时创建，优先级 1，静态栈 128 words。PA1/PA2/PA3 同时翻转，默认亮100ms、灭100ms，完整周期200ms。低电平点亮；电源指示灯无法软件控制。
- idle：内核空闲任务，静态栈 128 words。禁用动态分配，不链接 heap_*.c。
- 断言、任务栈溢出、HAL Error_Handler 和处理器故障进入停止路径，关闭脉冲并挂起，g_rtos_fault_file/line 可用调试器检查。未增加自动恢复。
- 原有复位后一次定量脉冲动作保留，连续运动开关不变。闪灯不是纯灯测试固件，首次上板请断开驱动器信号。
- 本轮不改变 ESP 固件、接线或靶车目录。

## 宏与 CMake

- 配置入口：App/RTOS/app_rtos_config.h、FreeRTOSConfig.h、cmake/freertos.cmake。
- TARGETCARRIER_LED_BLINK：默认 OFF（主工程默认不闪灯），转换成 APP_LED_BLINK_ENABLED 编译宏。
- TARGETCARRIER_LED_TOGGLE_MS：默认100，转换成 APP_LED_TOGGLE_MS；1..60000ms，改成250则亮250ms、灭250ms。
- 运行中修改参数不生效；宏变化需要重新编译和烧录。
- 需要 ARM GCC、CMake、Ninja 在本进程 PATH 可用，在本 STM32 目录中执行：

```powershell
cmake --preset Debug
cmake --build --preset Debug
cmake --preset Release
cmake --build --preset Release
# 可选覆盖灯的默认配置
cmake --preset Debug -DTARGETCARRIER_LED_BLINK=ON -DTARGETCARRIER_LED_TOGGLE_MS=100
```

每个 build/Debug 或 build/Release 下生成 stmc8t6.elf、.hex、.bin 和 .map；Flash 基址0x08000000。

## 中断与 CubeMX

- SysTick=1kHz，共用 HAL 与 FreeRTOS。SysTick_Handler 始终 HAL_IncTick；调度开始后才调用 xPortSysTickHandler，避免启动前触发调度器。
- SVC/PendSV 由内核直接提供，向量检查启用；SysTick/PendSV 优先级15，USART3优先级5，NVIC_PRIORITYGROUP_4。现有 USART ISR不调用RTOS API，也不做阻塞HAL等待。
- 临界区会短暂推迟共享 tick；此计时不作为精密波形时基，PUL仍由TIM1硬件输出。不允许在ISR中用HAL_Delay或依赖SysTick超时的阻塞操作。
- 原生内核由自定义CMake维护，未在.ioc中虚构CubeMX中间件配置。.ioc 禁止生成SVC/PendSV空函数，移植钩子置于USER CODE区域，App/RTOS独立于生成代码。
- CubeMX再生成后必须重新构建并核对main中AppRTOS_Start、SysTick包装和SVC/PendSV归属。CMake检测到重复SVC/PendSV函数会拒绝构建；不要改回空中断处理函数。

## 验证与恢复

临时ASCII目录中已通过两份Debug构建、18组主机测试及ELF向量/任务链接检查；从实际项目目录的源码快照构建的最终Debug/Release结果见 verification/freertos/RESULT.json 和构建日志。测试不证明上板调度、UART、电机或闪灯运行正常，本轮未烧录。

修改前STM32源码备份：_backups/freertos_20261019_154357/STM32_before.zip；根README也已备份。恢复时先保存后续修改，再将备份解压到独立位置核对；不要直接覆盖现有工作。
COPY_MANIFEST.csv 是最初复制时的历史记录，不代表本次移植后的文件哈希。

## 本机中文路径构建方式

实测本机 CMake 4.0.3 在含中文的源路径启动时以3221226505异常退出，无诊断输出。因此使用 tools/build_stm32.py 自动创建临时ASCII源码快照，构建后复制产物及日志回本项目build/Debug、build/Release。快照逐文件核验，构建期间源码变动会拒绝交付产物。保留临时源码便于调试器定位，具体路径见BUILD_RESULT.json；没有复制或复用旧CMake缓存。

工具已在PATH时，在本STM32工程目录执行：

```powershell
python tools/build_stm32.py --config Debug
python tools/build_stm32.py --config Release
# 示例：覆盖LED宏
python tools/build_stm32.py --config Debug --led ON --led-ms 100
```

工具不在PATH时使用 --cmake、--gcc、--ninja 分别传入可执行文件完整路径。临时目录若包含中文，使用 --work-root 指定已有的ASCII路径目录。本工具只修改子进程PATH，不更改系统环境变量或注册表。
