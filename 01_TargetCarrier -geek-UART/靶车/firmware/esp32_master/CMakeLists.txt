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
        src/param_store.cpp
        src/ws_proto.c
        src/ws_server.cpp
        src/display_lcd.c
        src/web_ui.cpp
    LIBS
        protocol
        linkcore
        netcfg
)

# 本工程的私有头文件目录
target_include_directories(firmware PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_BINARY_DIR}"          # 放生成的 web_index.h
)

# ---- 网页: 把 web/index.html 生成成 C 字符串头文件 --------------------------
# 页面是"组件化"的独立文件 (web/index.html), 不再塞在 .cpp 里;
# 改完 index.html 下一次构建会自动重跑 CMake 并重新生成 web_index.h。
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html" _WEB_INDEX_HTML)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/web/vue.global.prod.js" _WEB_VUE_JS)
string(LENGTH "${_WEB_INDEX_HTML}" _WEB_INDEX_LEN)
string(LENGTH "${_WEB_VUE_JS}" _WEB_VUE_LEN)

configure_file("${CMAKE_CURRENT_SOURCE_DIR}/web/index_html.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/web_index.h" @ONLY)
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/web/vue_js.h.in"
               "${CMAKE_CURRENT_BINARY_DIR}/web_vue.h" @ONLY)

set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/web/index.html"
    "${CMAKE_CURRENT_SOURCE_DIR}/web/vue.global.prod.js")

message(STATUS "   网页       : index.html ${_WEB_INDEX_LEN} 字符 (Vue), vue.js ${_WEB_VUE_LEN} 字符")

# 共享库也要拿到 ESP32 这套编译选项 (与原来 PlatformIO 编译它们时一致)
esp32s3_arduino_apply_flags(protocol)
esp32s3_arduino_apply_flags(linkcore)
esp32s3_arduino_apply_flags(netcfg)

# ---- 网页控制用到的框架库 ----------------------------------------------------
# WebServer 是框架自带库, 但默认没编; 用同一个助手把它的源码编进来。
# 它要用 WiFiServer/WiFiClient, 所以必须链到已经编好的 WiFi 库 (拿到头文件路径)。
esp32s3_arduino_add_library(esp32s3_arduino_webserver WebServer)
target_link_libraries(esp32s3_arduino_webserver PUBLIC esp32s3_arduino_wifi)
target_include_directories(esp32s3_arduino_webserver PUBLIC
    "${PIO_PACKAGES_DIR}/framework-arduinoespressif32/libraries/FS/src")
target_link_libraries(firmware PRIVATE esp32s3_arduino_webserver)

message(STATUS "----------------------------------------------------------")
message(STATUS " 靶车 · ESP32 主机 (ESP-NOW 发射端)")
message(STATUS "   变体      : ${ESP32S3_VARIANT}")
message(STATUS "   产物      : ${CMAKE_BINARY_DIR}/firmware.bin")
message(STATUS "----------------------------------------------------------")
