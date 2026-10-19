# =============================================================================
#  靶车 —— ESP32 主机 (ESP-NOW 发射端)   CMake 工程
#
#  VS Code 用法 (推荐):
#      1. 用 VS Code 打开 **firmware/esp32_master** 这个文件夹
#      2. 装扩展: CMake Tools (ms-vscode.cmake-tools) + C/C++ (ms-vscode.cpptools)
#      3. 底部状态栏点 "未选择工具包" -> 选预设 "esp32-master"
#      4. 点 Build (F7) 编译; 点 Flash 或运行任务 "flash" 烧录
#
#  命令行等价:
#      cmake --preset esp32-master
#      cmake --build --preset esp32-master
#      cmake --build --preset esp32-master --target flash     # 需要接板子
#      cmake --build --preset esp32-master --target monitor   # 看串口
#
#  板卡: M5Stack StampS3 (ESP32-S3, 8MB Flash)
#  串口波特率: 460800 (见 include/app_config.h 的 SERIAL_BAUD)
# =============================================================================

cmake_minimum_required(VERSION 3.25)

# 工具链文件必须先于 project() 生效, 所以这里做一次显式检查, 避免用户在
# 没走 preset 的情况下莫名其妙地"用宿主机 gcc 编 ESP32"。
if(NOT CMAKE_TOOLCHAIN_FILE)
    message(FATAL_ERROR
        "没有指定 CMake 工具链。请使用预设:\n"
        "    cmake --preset esp32-master\n"
        "或者显式指定:\n"
        "    cmake -S . -B build \\\n"
        "          -DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/toolchain-esp32s3.cmake")
endif()

project(esp32_master LANGUAGES C CXX ASM)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../cmake")
include(pio_sdk)          # 定位 PlatformIO 的 packages 目录
include(esp32_arduino)    # arduino-esp32 / ESP-IDF 的 CMake 支持

# ---- 共享库 ----------------------------------------------------------------
add_subdirectory("${CMAKE_CURRENT_SOURCE_DIR}/../lib" lib)

# 主机端打印串口是 460800 (见 include/app_config.h 的 SERIAL_BAUD)
set(ESP32S3_MONITOR_BAUD "460800" CACHE STRING "monitor 目标的串口波特率" FORCE)

# ---- 固件 ------------------------------------------------------------------
esp32s3_arduino_firmware(
    SOURCES
        src/main.cpp
        src/motion_profile.c
    LIBS
        protocol
)

# 本工程的私有头文件目录
target_include_directories(firmware PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

# 共享库也要拿到 ESP32 这套编译选项 (与原来 PlatformIO 编译它们时一致)
esp32s3_arduino_apply_flags(protocol)

message(STATUS "----------------------------------------------------------")
message(STATUS " 靶车 · ESP32 主机 (ESP-NOW 发射端)")
message(STATUS "   变体      : ${ESP32S3_VARIANT}")
message(STATUS "   产物      : ${CMAKE_BINARY_DIR}/firmware.bin")
message(STATUS "----------------------------------------------------------")
