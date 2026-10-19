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
        oled
)

target_include_directories(firmware PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

esp32s3_arduino_apply_flags(protocol)
esp32s3_arduino_apply_flags(oled)

message(STATUS "----------------------------------------------------------")
message(STATUS " 靶车 · M5Stamp S3 桥接端 (ESP-NOW -> UART)")
message(STATUS "   变体      : ${ESP32S3_VARIANT}")
message(STATUS "   产物      : ${CMAKE_BINARY_DIR}/firmware.bin")
message(STATUS "----------------------------------------------------------")
