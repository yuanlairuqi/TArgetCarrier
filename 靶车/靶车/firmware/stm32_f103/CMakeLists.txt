# =============================================================================
#  靶车 —— STM32F103C8T6 控制器 (MIT 闭环 + CAN)   CMake 工程
#
#  VS Code 用法 (推荐):
#      1. 用 VS Code 打开 **firmware/stm32_f103** 这个文件夹
#      2. 装扩展: CMake Tools + C/C++ (+ 可选 Cortex-Debug)
#      3. 状态栏选预设 "stm32-f103" -> Build (F7)
#      4. 运行任务 "flash" 用 ST-Link 烧录
#
#  命令行等价:
#      cmake --preset stm32-f103
#      cmake --build --preset stm32-f103
#      cmake --build --preset stm32-f103 --target flash
#
#  板卡: 野火 F103C8T6 核心板 (= 蓝板 bluepill 同芯片同引脚)
#        72MHz / 64KB Flash / 20KB RAM / 板载 8MHz 晶振
#
#  ★★★ 构建目录必须放在纯 ASCII 路径下 ★★★
#  ---------------------------------------------------------------------------
#  不是洁癖, 是 Windows 上 GNU binutils 的硬限制。
#
#  [PlatformIO 时代的对照实验] 同一份代码、同一套工具链:
#      A. 项目内 .pio, 中文路径, 相对路径   -> 失败 (cannot open map file)
#      B. 项目内 .pio, 中文路径, 绝对路径   -> 失败
#      C. C:/pio_build/ascii                -> 成功
#      D. C:/Users/zzq/Desktop/靶车/build   -> 失败 (短路径, 但仍含中文)
#      E. 超长纯 ASCII 路径                 -> 成功 (排除"路径太长")
#  结论: 与长度无关, 与路径里有没有非 ASCII 字符 (中文) 直接相关。
#
#  [换成 CMake 后精确定位到根因] 失败的不是 ld, 而是 **arm-none-eabi-objcopy**:
#      arm-none-eabi-objcopy.exe: C:/Users/zzq/Desktop/<乱码>/.../firmware.elf:
#                                 No such file or directory
#  链接本身是成功的 (firmware.elf 和 firmware.map 都正常生成了), 是后面
#  "ELF -> .bin/.hex" 这一步的 objcopy 打不开带中文的路径 —— 它按 ANSI 代码页
#  解析命令行参数, 中文就变成了乱码文件名。
#
#  这也顺带解释了为什么 ESP32 那边可以放在中文目录下: 它的镜像由 esptool.py
#  (Python, 内部走 Unicode API) 生成, 不经过 binutils。
#
#  所以: 源码可以继续放在中文目录, 只有 STM32 的**构建目录**需要是 ASCII,
#  CMakePresets.json 里已指到 C:/cmake_build/target-vehicle/stm32。
# =============================================================================

cmake_minimum_required(VERSION 3.25)

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "没有指定 CMake 工具链。请使用预设:\n"
        "    cmake --preset stm32-f103\n"
        "或者显式指定:\n"
        "    cmake -S . -B C:/cmake_build/target-vehicle/stm32 \\\n"
        "          -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain-stm32f103.cmake")
endif()

project(stm32_f103 LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(pio_sdk)
include(stm32_arduino)

# 构建目录含非 ASCII 字符时提前给出人话警告 (而不是让 ld 报一句莫名的错)
string(REGEX MATCH "[^ -~]" _non_ascii "${CMAKE_BINARY_DIR}")
if(_non_ascii)
    message(WARNING
        "构建目录包含非 ASCII 字符:\n    ${CMAKE_BINARY_DIR}\n"
        "arm-none-eabi-ld 在这种路径下无法写出 map 文件, 链接会失败。\n"
        "请改用 CMakePresets.json 里的 stm32-f103 预设 (它把构建目录放到 C:/cmake_build/...)。")
endif()
unset(_non_ascii)

# ---- 共享库 ----------------------------------------------------------------
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib" lib)

# ---- 固件 ------------------------------------------------------------------
stm32_arduino_firmware(
    SOURCES
        src/main.cpp
        src/app_main.c
        src/can_drv.c
        src/dm_motor.c
        src/link_uart.cpp
        src/timer_drv.cpp
        src/display_stm32.c
        src/oled_hal_stm32.c
    LIBS
        protocol
        oled
)

target_include_directories(firmware PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

stm32_arduino_apply_flags(protocol)
stm32_arduino_apply_flags(oled)

message(STATUS "----------------------------------------------------------")
message(STATUS " 靶车 · STM32F103C8T6 控制器 (MIT 闭环 + CAN)")
message(STATUS "   芯片      : ${STM32_CHIP}   Flash ${STM32_FLASH_SIZE}B / RAM ${STM32_RAM_SIZE}B")
message(STATUS "   产物      : ${CMAKE_BINARY_DIR}/firmware.bin")
message(STATUS "----------------------------------------------------------")
