# =============================================================================
#  靶车 —— M5Stamp S3 桥接端 (ESP-NOW -> UART)   CMake 工程
#
#  VS Code 用法 (推荐):
#      1. 用 VS Code 打开 **firmware/esp32_bridge** 这个文件夹
#      2. 装扩展: CMake Tools + C/C++
#      3. 状态栏选预设 "esp32-bridge" -> Build (F7)
#      4. 运行任务 "flash" 烧录; 运行任务 "monitor" 看串口
#
#  命令行等价:
#      cmake --preset esp32-bridge
#      cmake --build --preset esp32-bridge
#      cmake --build --preset esp32-bridge --target flash
#      cmake --build --preset esp32-bridge --target monitor
#
#  板卡: M5Stack StampS3 (ESP32-S3, 8MB Flash)
#  调试串口: 115200 (USB CDC, 不是接 STM32 的那一路)
# =============================================================================

cmake_minimum_required(VERSION 3.25)

if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "没有指定 CMake 工具链。请使用预设:\n"
        "    cmake --preset esp32-bridge\n"
        "或者显式指定:\n"
        "    cmake -S . -B build \\\n"
        "          -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain-esp32s3.cmake")
endif()

project(esp32_bridge LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(pio_sdk)
include(esp32_arduino)

# ---- 共享库 ----------------------------------------------------------------
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib" lib)

# ---- 固件 ------------------------------------------------------------------
esp32s3_arduino_firmware(
    SOURCES
        src/main.cpp
        src/display_esp32.cpp
        src/oled_hal_esp32.cpp
    LIBS
        protocol
        linkcore
        netcfg
        oled
)

target_include_directories(firmware PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# 桥接这块 OLED: 相对常见模块是"上下颠倒 + 左右反"的装法, 所以段方向设 0xA1、
# COM 扫描设 0xC0 (整体相对 0xA0/0xC8 旋转 180°)。
# 想改方向只需要动这两行: 0xA0/0xC8 = 正向, 0xA1/0xC8 = 只左右反, 0xA1/0xC0 = 旋转 180°
target_compile_definitions(oled PUBLIC OLED_SEG_REMAP=0xA1 OLED_COM_SCAN=0xC0)

esp32s3_arduino_apply_flags(protocol)
esp32s3_arduino_apply_flags(linkcore)
esp32s3_arduino_apply_flags(oled)
esp32s3_arduino_apply_flags(netcfg)

message(STATUS "----------------------------------------------------------")
message(STATUS " 靶车 · M5Stamp S3 桥接端 (ESP-NOW -> UART)")
message(STATUS "   变体      : ${ESP32S3_VARIANT}")
message(STATUS "   产物      : ${CMAKE_BINARY_DIR}/firmware.bin")
message(STATUS "----------------------------------------------------------")
