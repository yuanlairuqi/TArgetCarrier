# =============================================================================
#  esp32_arduino.cmake —— 用 CMake 构建 arduino-esp32 (ESP32-S3) 固件
#
#  它做的事, 等价于原来 PlatformIO 的这几步:
#      1. 把 cores/esp32 的源码编成 libarduino_core.a   (PIO: libFrameworkArduino.a)
#      2. 把 libraries/WiFi 编成 libarduino_WiFi.a      (PIO: libWiFi.a)
#      3. 把工程 src/ 与共享库编译、链接成 firmware.elf
#      4. 用 esptool 生成 firmware.bin / bootloader.bin / partitions.bin
#      5. 提供 flash / erase / monitor / size 四个自定义目标
#
#  依赖的 PlatformIO 包 (已装好即可, 不再下载):
#      toolchain-xtensa-esp32s3      交叉编译器
#      framework-arduinoespressif32  arduino-esp32 2.0.17 + 预编译 IDF 4.4.7
#      tool-esptoolpy                esptool (生成镜像 / 烧录)
#
#  用法见 firmware/esp32_master/CMakeLists.txt
# =============================================================================

# ---- 1. 路径与基本参数 -------------------------------------------------------
# ★ 顺序很重要: 必须先定下 ESP32S3_ARDUINO_ROOT, 再 include 数据表文件。
#   数据表里的 `${ESP32S3_ARDUINO_ROOT}/...` 是在 include 的瞬间就展开的,
#   如果反过来, 那 199 条头文件路径会变成 "/tools/sdk/..." 这种空根路径,
#   编译时报 sdkconfig.h: No such file or directory。
set(ESP32S3_ARDUINO_ROOT "${PIO_PACKAGES_DIR}/framework-arduinoespressif32"
    CACHE PATH "arduino-esp32 框架根目录")
set(ESP32S3_SDK_DIR "${ESP32S3_ARDUINO_ROOT}/tools/sdk/esp32s3")
set(ESP32S3_VARIANT "m5stack_stamp_s3" CACHE STRING "Arduino 变体 (引脚定义) 名称")

include("${CMAKE_CURRENT_LIST_DIR}/esp32s3_arduino_sdk.cmake")

# 源码路径合法性提示 (中文目录会让 ninja 的增量编译失效, 只警告不报错)
vehicle_check_ascii_source_path()

# ---- 2. 屏蔽 CMake 内置的构建类型选项 ----------------------------------------
# CMAKE_BUILD_TYPE=Release 会自动追加 "-O3 -DNDEBUG", 而 PlatformIO 的 ESP32
# 构建只有 "-Os"、没有 NDEBUG。两者的差别不是性能而是 assert 行为, 所以这里
# 把内置的 Debug/Release/... 选项全部清空, 优化级别只由 ESP32S3_COMMON_FLAGS
# 里的 -Os 决定 —— 与平台无关, 结果可复现。
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_C_FLAGS_${_cfg}   "" CACHE STRING "有意留空: 优化选项统一在 ESP32S3_COMMON_FLAGS 里" FORCE)
    set(CMAKE_CXX_FLAGS_${_cfg} "" CACHE STRING "有意留空: 优化选项统一在 ESP32S3_COMMON_FLAGS 里" FORCE)
endforeach()
unset(_cfg)

# ---- 3. 检查包齐不齐 ---------------------------------------------------------
pio_require_package(framework-arduinoespressif32)
pio_require_package(tool-esptoolpy)

set(ESP32S3_CORE_DIR    "${ESP32S3_ARDUINO_ROOT}/cores/esp32")
set(ESP32S3_VARIANT_DIR "${ESP32S3_ARDUINO_ROOT}/variants/${ESP32S3_VARIANT}")
set(ESP32S3_ESPTOOL     "${PIO_PACKAGES_DIR}/tool-esptoolpy/esptool.py")
set(ESP32S3_GENPART     "${ESP32S3_ARDUINO_ROOT}/tools/gen_esp32part.py")

if(NOT EXISTS "${ESP32S3_VARIANT_DIR}")
    message(FATAL_ERROR "找不到 Arduino 变体目录: ${ESP32S3_VARIANT_DIR}")
endif()

# ---- 4. 目标机参数 (与 PlatformIO 一致) --------------------------------------
# 板卡: M5Stack StampS3 = ESP32-S3, 8MB Flash, 分区表 default_8MB.csv
set(ESP32S3_FLASH_MODE "dio" CACHE STRING "flash 模式")
set(ESP32S3_FLASH_FREQ "80m" CACHE STRING "flash 频率")
set(ESP32S3_FLASH_SIZE "8MB" CACHE STRING "flash 容量")
set(ESP32S3_PART_CSV   "${ESP32S3_ARDUINO_ROOT}/tools/partitions/default_8MB.csv")

# 二级引导镜像用框架里预编译好的 qio 版本。
# ★ 别把它"统一"成 ${ESP32S3_FLASH_MODE}: PlatformIO 当年就是这个组合 ——
#   取 bootloader_qio_80m.elf (来自板卡配置 flash_mode=qio), 而 elf2image 时
#   又传 --flash_mode dio。两边都照抄才和原产物一致 (生成的 bootloader.bin
#   已核对与原构建 SHA256 完全相同), 看着别扭但对。
set(ESP32S3_BOOTLOADER_ELF
    "${ESP32S3_ARDUINO_ROOT}/tools/sdk/esp32s3/bin/bootloader_qio_80m.elf")

# monitor 目标的波特率。注意这不是烧录速率, 而是板子打印串口的速率:
#   主机端 460800 (见 include/app_config.h 的 SERIAL_BAUD)
#   桥接端 115200 (USB CDC 调试口)
# 各工程在自己的 CMakeLists.txt 里覆盖这个变量。
set(ESP32S3_MONITOR_BAUD "115200" CACHE STRING "monitor 目标的串口波特率")

# ---- 5. 公共编译属性 (头文件 / 宏 / 选项) ------------------------------------
# core、框架库、应用三者共用同一套, 与 PlatformIO 的 build_flags 完全对齐
function(esp32s3_arduino_apply_flags tgt)
    target_include_directories(${tgt} PUBLIC
        ${ESP32S3_IDF_INCLUDE_DIRS}
        "${ESP32S3_CORE_DIR}"
        "${ESP32S3_VARIANT_DIR}"
    )
    target_compile_definitions(${tgt} PUBLIC ${ESP32S3_DEFINES})
    target_compile_options(${tgt} PUBLIC
        $<$<COMPILE_LANGUAGE:C>:-std=gnu99>
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++11>
        $<$<COMPILE_LANGUAGE:CXX>:-fexceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        ${ESP32S3_COMMON_FLAGS}
    )
endfunction()

# ---- 6. Arduino 核心库 (cores/esp32) ----------------------------------------
# ★ 故意不用 file(GLOB ... CONFIGURE_DEPENDS): 它会在每次构建时插入一个
#   cmake.verify_globs 检查, 实测让 ninja 每次都说 "Re-running CMake", 平白
#   多花时间。代价是新增**框架**源文件后要手动重新 configure 一次;
#   本项目自己的源文件都在 CMakeLists 里显式列出, 不受影响。
file(GLOB ESP32S3_CORE_SOURCES
    "${ESP32S3_CORE_DIR}/*.c"
    "${ESP32S3_CORE_DIR}/*.cpp"
    "${ESP32S3_CORE_DIR}/libb64/*.c"
    "${ESP32S3_CORE_DIR}/libb64/*.cpp"
)
list(LENGTH ESP32S3_CORE_SOURCES _n)
if(_n LESS 40)
    message(FATAL_ERROR
        "cores/esp32 只找到 ${_n} 个源文件, 框架目录可能不完整: ${ESP32S3_CORE_DIR}")
endif()
unset(_n)

add_library(esp32s3_arduino_core STATIC ${ESP32S3_CORE_SOURCES})
# core 独有: -DARDUINO_CORE_BUILD (应用与库都不定义它, 与 PlatformIO 一致)
target_compile_definitions(esp32s3_arduino_core PRIVATE ${ESP32S3_CORE_ONLY_DEFINES})
esp32s3_arduino_apply_flags(esp32s3_arduino_core)
set_target_properties(esp32s3_arduino_core PROPERTIES OUTPUT_NAME "arduino_core")

# ---- 7. 框架库 (libraries/*) 构建助手 ----------------------------------------
#
#   esp32s3_arduino_add_library(<目标名> <库名>)
#       <库名> = framework-arduinoespressif32/libraries/<库名>
#   只编译工程真正用到的库 (PlatformIO 的 LDF 做的是同一件事)
#
function(esp32s3_arduino_add_library tgt libname)
    set(_dir "${ESP32S3_ARDUINO_ROOT}/libraries/${libname}/src")
    if(NOT EXISTS "${_dir}")
        message(FATAL_ERROR "找不到 Arduino 库 '${libname}': ${_dir}")
    endif()
    file(GLOB_RECURSE _srcs
        "${_dir}/*.c" "${_dir}/*.cpp" "${_dir}/*.S")
    add_library(${tgt} STATIC ${_srcs})
    target_include_directories(${tgt} PUBLIC "${_dir}")
    esp32s3_arduino_apply_flags(${tgt})
    set_target_properties(${tgt} PROPERTIES OUTPUT_NAME "arduino_${libname}")
endfunction()

# WiFi: 主机与桥接端都要用 (ESP-NOW 依赖它)
esp32s3_arduino_add_library(esp32s3_arduino_wifi WiFi)

# ---- 8. 固件目标 ------------------------------------------------------------
#
#   esp32s3_arduino_firmware(SOURCES <应用源文件...> LIBS <库目标...>)
#
#   产物 (都在构建目录里, 布局与 PlatformIO 的 .pio/build/<env>/ 一致):
#       firmware.elf / firmware.map
#       firmware.bin        应用镜像, 烧到 0x10000
#       bootloader.bin      二级引导, 烧到 0x0
#       partitions.bin      分区表,   烧到 0x8000
#       boot_app0.bin       启动标记, 烧到 0xe000
#
function(esp32s3_arduino_firmware)
    cmake_parse_arguments(FW "" "" "SOURCES;LIBS" ${ARGN})
    set(tgt firmware)

    add_executable(${tgt} ${FW_SOURCES})
    # 裸机镜像固定叫 firmware.elf (CMake 在 Generic 平台上不会自动加后缀)
    set_target_properties(${tgt} PROPERTIES SUFFIX ".elf")
    esp32s3_arduino_apply_flags(${tgt})

    # ---- 链接脚本: 顺序与 PlatformIO 完全一致 ----
    # memory.ld / rom*.ld 在 sdk/ld 下; sections.ld 在 sdk/qio_qspi 下
    set(_ld_scripts
        "${ESP32S3_SDK_DIR}/ld/memory.ld"
        "${ESP32S3_SDK_DIR}/qio_qspi/sections.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.rom.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.rom.api.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.rom.libgcc.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.rom.newlib.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.rom.version.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.rom.newlib-time.ld"
        "${ESP32S3_SDK_DIR}/ld/esp32s3.peripherals.ld"
    )
    set(_T "")
    foreach(s IN LISTS _ld_scripts)
        if(NOT EXISTS "${s}")
            message(FATAL_ERROR "缺少链接脚本: ${s}")
        endif()
        list(APPEND _T "-T${s}")
    endforeach()

    target_link_directories(${tgt} PRIVATE
        "${ESP32S3_SDK_DIR}/lib"
        "${ESP32S3_SDK_DIR}/ld"
        "${ESP32S3_SDK_DIR}/qio_qspi"
    )

    # IDF 的预编译静态库全量参与链接 (与 PlatformIO 相同), 靠 --gc-sections 裁剪
    file(GLOB _idf_archives
        "${ESP32S3_SDK_DIR}/lib/*.a"
        "${ESP32S3_SDK_DIR}/ld/*.a"
        "${ESP32S3_SDK_DIR}/qio_qspi/*.a"
    )
    list(LENGTH _idf_archives _n_ar)
    message(STATUS "esp32s3-arduino: 链接 ${_n_ar} 个 IDF 预编译库")
    unset(_n_ar)

    target_link_libraries(${tgt} PRIVATE
        ${FW_LIBS}
        esp32s3_arduino_wifi
        esp32s3_arduino_core
        # 静态库互相引用, 用 RESCAN 组包 (等价 ld 的 --start-group/--end-group)
        "$<LINK_GROUP:RESCAN,${_idf_archives}>"
        # 工具链自带的运行库 / 数学库 / C++ 标准库
        c m stdc++ gcc
    )

    target_link_options(${tgt} PRIVATE
        ${_T}
        -mlongcalls
        -Wl,--cref
        -Wl,--gc-sections
        -fno-rtti
        -fno-lto
        # Arduino core 依赖 IDF 的这几个包装函数
        -Wl,--wrap=esp_log_write
        -Wl,--wrap=esp_log_writev
        -Wl,--wrap=log_printf
        -Wl,--wrap=longjmp
        -Wl,--undefined=uxTopUsedPriority
        # 下面这一串 -u 保证链接器不把 Arduino 骨架与 IDF 初始化代码裁掉
        -Wl,-u,_Z5setupv
        -Wl,-u,_Z4loopv
        -Wl,-u,esp_app_desc
        -Wl,-u,pthread_include_pthread_impl
        -Wl,-u,pthread_include_pthread_cond_var_impl
        -Wl,-u,pthread_include_pthread_local_storage_impl
        -Wl,-u,pthread_include_pthread_rwlock_impl
        -Wl,-u,include_esp_phy_override
        -Wl,-u,ld_include_highint_hdl
        -Wl,-u,start_app
        -Wl,-u,start_app_other_cores
        -Wl,-u,__ubsan_include
        -Wl,-u,__assert_func
        -Wl,-u,vfs_include_syscalls_impl
        -Wl,-u,app_main
        -Wl,-u,newlib_include_heap_impl
        -Wl,-u,newlib_include_syscalls_impl
        -Wl,-u,newlib_include_pthread_impl
        -Wl,-u,newlib_include_assert_impl
        -Wl,-u,__cxa_guard_dummy
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/firmware.map"
    )

    # ---- 镜像生成 (每次链接完自动执行) ----
    set(_bin  "${CMAKE_CURRENT_BINARY_DIR}/firmware.bin")
    set(_boot "${CMAKE_CURRENT_BINARY_DIR}/bootloader.bin")
    set(_part "${CMAKE_CURRENT_BINARY_DIR}/partitions.bin")
    set(_app0 "${CMAKE_CURRENT_BINARY_DIR}/boot_app0.bin")

    add_custom_command(TARGET ${tgt} POST_BUILD
        BYPRODUCTS "${_bin}" "${_boot}" "${_part}" "${_app0}"
        COMMENT "生成 ESP32-S3 镜像 (firmware.bin / bootloader.bin / partitions.bin)"
        COMMAND "${PIO_PYTHON}" "${ESP32S3_ESPTOOL}" --chip esp32s3 elf2image
                --flash_mode ${ESP32S3_FLASH_MODE} --flash_freq ${ESP32S3_FLASH_FREQ}
                --flash_size ${ESP32S3_FLASH_SIZE}
                -o "${_boot}" "${ESP32S3_BOOTLOADER_ELF}"
        COMMAND "${PIO_PYTHON}" "${ESP32S3_GENPART}" -q "${ESP32S3_PART_CSV}" "${_part}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ESP32S3_ARDUINO_ROOT}/tools/partitions/boot_app0.bin" "${_app0}"
        COMMAND "${PIO_PYTHON}" "${ESP32S3_ESPTOOL}" --chip esp32s3 elf2image
                --flash_mode ${ESP32S3_FLASH_MODE} --flash_freq ${ESP32S3_FLASH_FREQ}
                --flash_size ${ESP32S3_FLASH_SIZE}
                --elf-sha256-offset 0xb0
                -o "${_bin}" "$<TARGET_FILE:${tgt}>"
        VERBATIM
    )

    # ---- 串口参数: -DESP32_PORT=COM5 / 环境变量 ESP32_PORT / 自动探测 ----
    set(_port "$ENV{ESP32_PORT}")
    if(DEFINED ESP32_PORT AND NOT "${ESP32_PORT}" STREQUAL "")
        set(_port "${ESP32_PORT}")
    endif()
    if(_port)
        set(_port_args -p "${_port}")
        set(_port_text "${_port}")
    else()
        set(_port_args "")
        set(_port_text "自动探测")
    endif()

    # ---- flash: 烧录 ----
    add_custom_target(flash
        COMMAND "${PIO_PYTHON}" "${ESP32S3_ESPTOOL}" --chip esp32s3 ${_port_args} -b 460800
                --before default_reset --after hard_reset
                write_flash
                0x0     "${_boot}"
                0x8000  "${_part}"
                0xe000  "${_app0}"
                0x10000 "${_bin}"
        DEPENDS ${tgt}
        COMMENT "烧录固件 (串口: ${_port_text})"
        USES_TERMINAL
        VERBATIM
    )

    # ---- erase: 整片擦除 ----
    add_custom_target(erase
        COMMAND "${PIO_PYTHON}" "${ESP32S3_ESPTOOL}" --chip esp32s3 ${_port_args} erase_flash
        COMMENT "整片擦除 Flash (串口: ${_port_text})"
        USES_TERMINAL
        VERBATIM
    )

    # ---- monitor: 串口监视 ----
    # ★ 这里必须用 CMAKE_CURRENT_FUNCTION_LIST_DIR 而不是 CMAKE_CURRENT_LIST_DIR:
    #   在函数体里, CMAKE_CURRENT_LIST_DIR 指向的是**调用者**的 CMakeLists.txt
    #   所在目录 (即 firmware/esp32_master), 拼出来会变成
    #   firmware/tools/serial_monitor.py —— 不存在的路径。
    add_custom_target(monitor
        COMMAND "${PIO_PYTHON}" "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../tools/serial_monitor.py"
                ${_port_args} --baud ${ESP32S3_MONITOR_BAUD}
        COMMENT "串口监视 ${_port_text} @ ${ESP32S3_MONITOR_BAUD} (Ctrl+C 退出)"
        USES_TERMINAL
        VERBATIM
    )

    # ---- size: Flash / RAM 占用 ----
    add_custom_target(size
        COMMAND "${CMAKE_SIZE}" --format=berkeley "$<TARGET_FILE:${tgt}>"
        DEPENDS ${tgt}
        COMMENT "Flash / RAM 占用"
        VERBATIM
    )
endfunction()
