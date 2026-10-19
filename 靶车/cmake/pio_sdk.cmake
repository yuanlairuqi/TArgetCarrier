# =============================================================================
#  pio_sdk.cmake —— 定位 PlatformIO 已经装好的"SDK 包"
#
#  为什么 CMake 还要依赖 PlatformIO 的目录？
#  ---------------------------------------------------------------
#  本项目的三块板子都要 Arduino 核心库：
#      ESP32-S3   -> framework-arduinoespressif32 (内含 arduino-esp32 2.0.17
#                    + 预编译的 ESP-IDF 4.4.7 静态库和链接脚本)
#      STM32F103  -> framework-arduinoststm32    (STM32duino 2.x)
#                    + framework-cmsis
#  这些框架加起来几百 MB，官方也没有提供 CMake 包（ESP-IDF 有，但 arduino-esp32
#  的 "Arduino as IDF component" 走的是另一套构建流程，和本项目的 PlatformIO
#  配置不等价）。
#
#  所以这里采取的做法是：
#      「CMake 自己负责编译和链接，但复用 PlatformIO 下好的工具链与框架源码」
#  好处是：
#      1. 不用再下载几百 MB 的框架，机器上已有的东西直接用；
#      2. 编译参数、宏、头文件搜索路径和原来的 PlatformIO 构建**逐项对齐**
#         （见 cmake/esp32s3_arduino_sdk.cmake 里的说明），产物等价；
#      3. 想彻底脱离 PlatformIO 也很容易：把这两个 framework 目录和两套
#         toolchain 拷到任意位置，然后用 PIO_PACKAGES_DIR 指过去即可。
#
#  覆盖方式（按优先级从高到低）：
#      1. cmake -DPIO_PACKAGES_DIR=D:/sdk/.platformio/packages ..
#      2. 环境变量 PLATFORMIO_CORE_DIR  (指向 .platformio 目录本身)
#      3. 环境变量 PLATFORMIO_PACKAGES_DIR
#      4. ~/.platformio/packages   (Windows 上即 %USERPROFILE%\.platformio\packages)
# =============================================================================

# 这个文件会被工具链文件和各个 CMakeLists 重复 include, 加个幂等保护
if(DEFINED PIO_SDK_INCLUDED)
    return()
endif()
set(PIO_SDK_INCLUDED TRUE)

# ---- 1. 定位 packages 目录 ---------------------------------------------------
if(NOT DEFINED PIO_PACKAGES_DIR OR PIO_PACKAGES_DIR STREQUAL "")
    if(DEFINED ENV{PLATFORMIO_PACKAGES_DIR} AND NOT "$ENV{PLATFORMIO_PACKAGES_DIR}" STREQUAL "")
        set(_pio_hint "$ENV{PLATFORMIO_PACKAGES_DIR}")
    elseif(DEFINED ENV{PLATFORMIO_CORE_DIR} AND NOT "$ENV{PLATFORMIO_CORE_DIR}" STREQUAL "")
        set(_pio_hint "$ENV{PLATFORMIO_CORE_DIR}/packages")
    elseif(DEFINED ENV{USERPROFILE})
        set(_pio_hint "$ENV{USERPROFILE}/.platformio/packages")
    elseif(DEFINED ENV{HOME})
        set(_pio_hint "$ENV{HOME}/.platformio/packages")
    else()
        set(_pio_hint "")
    endif()
    if(NOT _pio_hint STREQUAL "")
        file(TO_CMAKE_PATH "${_pio_hint}" _pio_hint)
    endif()
else()
    file(TO_CMAKE_PATH "${PIO_PACKAGES_DIR}" _pio_hint)
endif()

set(PIO_PACKAGES_DIR "${_pio_hint}" CACHE PATH
    "PlatformIO 的 packages 目录 (内含 framework-arduinoespressif32 / framework-arduinoststm32 / 交叉工具链)")

if(NOT EXISTS "${PIO_PACKAGES_DIR}")
    message(FATAL_ERROR
        "找不到 PlatformIO 的 packages 目录: '${PIO_PACKAGES_DIR}'\n"
        "请先安装 PlatformIO (pip install platformio && pio pkg install)，\n"
        "或者用 -DPIO_PACKAGES_DIR=<路径> 明确指定。")
endif()

# ---- 2. 便捷变量 -------------------------------------------------------------
set(PIO_CORE_DIR "${PIO_PACKAGES_DIR}/.." CACHE INTERNAL "")

# PlatformIO 自带一个 Python 虚拟环境, esptool / gen_esp32part 都在里面
if(WIN32)
    set(_pio_py "${PIO_CORE_DIR}/penv/Scripts/python.exe")
else()
    set(_pio_py "${PIO_CORE_DIR}/penv/bin/python")
endif()
if(EXISTS "${_pio_py}")
    set(PIO_PYTHON "${_pio_py}" CACHE FILEPATH "PlatformIO 的 Python 解释器 (跑 esptool 用)")
else()
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_FOUND)
        set(PIO_PYTHON "${Python3_EXECUTABLE}" CACHE FILEPATH "Python 解释器 (跑 esptool 用)")
    endif()
endif()

# ---- 3. 检查某个包在不在, 不在就给出可读的报错 -------------------------------
function(pio_require_package pkg)
    if(NOT EXISTS "${PIO_PACKAGES_DIR}/${pkg}")
        message(FATAL_ERROR
            "缺少 PlatformIO 包 '${pkg}' (期望路径 ${PIO_PACKAGES_DIR}/${pkg})。\n"
            "在任意 PlatformIO 工程目录执行一次即可补齐:\n"
            "    pio pkg install --global --platform espressif32   # ESP32 相关\n"
            "    pio pkg install --global --platform ststm32       # STM32 相关")
    endif()
endfunction()

message(STATUS "PlatformIO packages : ${PIO_PACKAGES_DIR}")
if(DEFINED PIO_PYTHON)
    message(STATUS "PlatformIO python   : ${PIO_PYTHON}")
endif()

# =============================================================================
#  vehicle_check_ascii_source_path() —— 检查源码路径里有没有非 ASCII 字符
#
#  为什么要在意这件事:
#    Ninja 在 Windows 上处理不了"源码路径含非 ASCII 字符"的依赖信息 —— 它读完
#    gcc 生成的 .d 文件后, 无法把里面带中文的路径对应回文件, 于是每次都判定
#    "deps are missing", 结果**每次构建都把全部源文件重编一遍**(链接也跟着重来)。
#    实测对照: 同一份工程放在 C:\target-vehicle 下, 第二次构建输出
#    "ninja: no work to do."; 放在 C:\Users\zzq\Desktop\靶车\ 下, 第二次构建
#    仍然 [13/13] 全部重编。
#
#  这不是"能不能编过"的问题, 只是增量编译失效 (多花几十秒到几分钟), 所以这里
#  只警告不报错。想修的话: 把整个工程目录改成纯英文名 (例如 D:\target-vehicle)。
# =============================================================================
function(vehicle_check_ascii_source_path)
    if(NOT CMAKE_GENERATOR MATCHES "Ninja")
        return()
    endif()
    string(REGEX MATCH "[^ -~]" _vehicle_non_ascii "${CMAKE_SOURCE_DIR}")
    if(_vehicle_non_ascii)
        message(WARNING
            "\n"
            "  源码目录包含非 ASCII 字符 (中文):\n"
            "      ${CMAKE_SOURCE_DIR}\n"
            "\n"
            "  影响: Ninja 无法为这些路径记录依赖信息, 于是**每次构建都会全量重编**,\n"
            "        增量编译形同虚设 (STM32 多花约 40 秒, ESP32 多花几分钟)。\n"
            "        编译结果本身是正确的, 只是慢。\n"
            "\n"
            "  想要增量编译, 把工程目录改成纯英文名即可, 例如\n"
            "      D:\\target-vehicle\n"
            "  源码内容不用改, CMakeLists 也不用改 (路径全部是相对的)。\n")
    endif()
    unset(_vehicle_non_ascii)
endfunction()
