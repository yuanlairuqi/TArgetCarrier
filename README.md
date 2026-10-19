# TArgetCarrier

本工程创建于 2026-10-19。原工程只读复制，未修改源文件。

## 目录

- x轴/野火stm：野火 STM32F103C8T6 的独立 CMake 固件项目。
- x轴/esp：ESP32-S3 的独立 ESP-IDF CMake 固件项目。
- 靶车：预留空目录，本轮不实施。

两端硬件相同，沿用既有接线。S3 G4(TX)→STM32 PB11(RX)，G5(RX)←PB10(TX)，GND共地；STM32 PA8(PUL)→S3 G6，PB0(DIR)→S3 G7。

## 当前状态与后续要求

- STM32 已于2026-10-19移植 FreeRTOS-Kernel V11.2.0，详见 x轴/野火stm/FREERTOS_PORT.md。
- ESP32-S3 已使用 ESP-IDF 提供的 FreeRTOS。
- 两个固件分别构建，使用不同交叉工具链；没有合并为一个 CMake 编译目标。
- 后续要求通过宏配置功能和参数。现有 STM32 部分参数位于 App/Motion/motor_config.h；ESP 的 Kconfig 位于 main/Kconfig.projbuild。尚未补齐或统一宏配置。修改编译宏后需要重新构建并烧录。
- STM32继续使用CMake；本机中文路径请使用 x轴/野火stm/tools/build_stm32.py 构建，参数见 FREERTOS_PORT.md。
- ESP 使用现有 ESP-IDF 环境，通过 idf.py build 构建。当前目录含中文；既有工具链曾发生中文路径兼容问题，后续构建应准备独立 ASCII 路径副本并使用新构建目录。
- sdkconfig 和 sdkconfig.defaults 原样保留；未复制临时 ESP-NOW 校验配置或配对文件，未改变无线及运动开关。
- 原配置保留复位后一次定量脉冲动作，连续运动默认关闭，ESP-NOW默认关闭；未烧录、未进行硬件验证。

## 复制范围

仅复制两个固件的源码、驱动、CMake配置、STM32 .ioc/启动/链接文件和 ESP SDK配置。
未复制安卓、旧构建缓存、交付固件、Keil工程、原仓库Git历史、历史备份、配套文档和测试工具。
COPY_MANIFEST.csv 记录每个复制文件的源路径与 SHA-256；复制后逐文件校验目标内容，并复核对应原文件未变化。
以上复制记录为初始状态；2026-10-19已追加STM32 FreeRTOS移植，最终构建记录见 verification/freertos。

原工程：C:\Users\y\Desktop\靶车项目\11_电机控制完整项目_野火STM32F103C8T6双USB

## STM32 FreeRTOS 更新（2026-10-19）

详细任务、灯配置、构建方式和注意事项：[FREERTOS_PORT.md](x轴/野火stm/FREERTOS_PORT.md)。
本项目LED默认：OFF（主工程默认不闪灯）。ESP及靶车目录未改动。
