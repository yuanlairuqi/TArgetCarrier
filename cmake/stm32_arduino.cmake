# =============================================================================
#  stm32_arduino.cmake —— 用 CMake 构建 STM32F103C8T6 (STM32duino) 固件
#
#  等价于原来 PlatformIO 的 bluepill_f103c8 环境:
#      1. cores/arduino           -> libstm32_core.a   (PIO: libFrameworkArduino.a)
#      2. libraries/SrcWrapper    -> 目标文件直接进链接 (PIO 也是这么做的)
#      3. variants/STM32F1xx/...  -> 目标文件直接进链接
#      4. 工程 src/ + 共享库      -> firmware.elf
#      5. objcopy 出 .bin / .hex, 并给出 flash / size 目标
#
#  依赖的 PlatformIO 包:
#      toolchain-gccarmnoneeabi    arm-none-eabi 交叉编译器
#      framework-arduinoststm32    STM32duino 2.x
#      framework-cmsis             CMSIS Core / DSP
#      tool-openocd                烧录与调试
# =============================================================================

# ---- 1. 路径 -----------------------------------------------------------------
# 源码路径合法性提示 (中文目录会让 ninja 的增量编译失效, 只警告不报错)
vehicle_check_ascii_source_path()

set(STM32DUINO_ROOT "${PIO_PACKAGES_DIR}/framework-arduinoststm32"
    CACHE PATH "STM32duino 框架根目录")
set(CMSIS_ROOT "${PIO_PACKAGES_DIR}/framework-cmsis"
    CACHE PATH "CMSIS 根目录")

# 屏蔽 CMake 内置构建类型选项: 优化级别与 -DNDEBUG 统一由 STM32_COMMON_FLAGS
# 和 STM32_DEFINES 精确控制, 避免 CMake 再叠一层 -O3/-O2 把 -Os 顶掉。
foreach(_cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_C_FLAGS_${_cfg}   "" CACHE STRING "有意留空: 优化选项统一在 STM32_COMMON_FLAGS 里" FORCE)
    set(CMAKE_CXX_FLAGS_${_cfg} "" CACHE STRING "有意留空: 优化选项统一在 STM32_COMMON_FLAGS 里" FORCE)
endforeach()
unset(_cfg)

pio_require_package(framework-arduinoststm32)
pio_require_package(framework-cmsis)

# 变体目录名里带圆括号, CMake 里用引号包住没问题
set(STM32_VARIANT_DIR
    "${STM32DUINO_ROOT}/variants/STM32F1xx/F103C8T_F103CB(T-U)")

if(NOT EXISTS "${STM32_VARIANT_DIR}")
    message(FATAL_ERROR "找不到变体目录: ${STM32_VARIANT_DIR}")
endif()

# ---- 2. 目标芯片参数 ---------------------------------------------------------
# STM32F103C8T6: 64KB Flash / 20KB RAM / Cortex-M3 / 8MHz 外部晶振
set(STM32_CHIP             "STM32F103xB" CACHE STRING "芯片型号宏")
set(STM32_FLASH_SIZE       "65536"      CACHE STRING "Flash 字节数")
set(STM32_RAM_SIZE         "20480"      CACHE STRING "RAM 字节数")
set(STM32_FLASH_OFFSET     "0x0"        CACHE STRING "应用起始偏移")

# ---- 3. 头文件目录 (27 项, 顺序与 PlatformIO 一致) ---------------------------
set(STM32_INCLUDE_DIRS
    "${STM32DUINO_ROOT}/cores/arduino/avr"
    "${STM32DUINO_ROOT}/cores/arduino/stm32"
    "${STM32DUINO_ROOT}/cores/arduino/api/deprecated"
    "${STM32DUINO_ROOT}/libraries/SrcWrapper/inc"
    "${STM32DUINO_ROOT}/libraries/SrcWrapper/inc/LL"
    "${STM32DUINO_ROOT}/libraries/USBDevice/inc"
    "${STM32DUINO_ROOT}/libraries/VirtIO/inc"
    "${STM32DUINO_ROOT}/system/Drivers/STM32F1xx_HAL_Driver/Inc"
    "${STM32DUINO_ROOT}/system/Drivers/STM32F1xx_HAL_Driver/Src"
    "${STM32DUINO_ROOT}/system/STM32F1xx"
    "${STM32DUINO_ROOT}/system/Middlewares/ST/STM32_USB_Device_Library/Core/Inc"
    "${STM32DUINO_ROOT}/system/Middlewares/ST/STM32_USB_Device_Library/Core/Src"
    "${STM32DUINO_ROOT}/system/Middlewares/OpenAMP"
    "${STM32DUINO_ROOT}/system/Middlewares/OpenAMP/open-amp/lib/include"
    "${STM32DUINO_ROOT}/system/Middlewares/OpenAMP/libmetal/lib/include"
    "${STM32DUINO_ROOT}/system/Middlewares/OpenAMP/virtual_driver"
    "${CMSIS_ROOT}/CMSIS/Core/Include"
    "${STM32DUINO_ROOT}/system/Drivers/CMSIS/Device/ST/STM32F1xx/Include"
    "${STM32DUINO_ROOT}/system/Drivers/CMSIS/Device/ST/STM32F1xx/Source/Templates/gcc"
    "${CMSIS_ROOT}/CMSIS/DSP/Include"
    "${CMSIS_ROOT}/CMSIS/DSP/PrivateInclude"
    "${STM32DUINO_ROOT}/cores/arduino"
    "${STM32_VARIANT_DIR}"
)

# ---- 4. 预处理宏 -------------------------------------------------------------
set(STM32_DEFINES
    "STM32F1"
    "${STM32_CHIP}"
    "HSE_VALUE=8000000U"          # 野火核心板板载 8MHz 晶振
    "STM32F1xx"
    "ARDUINO=10808"
    "ARDUINO_ARCH_STM32"
    "NDEBUG"
    "ARDUINO_BLUEPILL_F103C8"
    "BOARD_NAME=\"BLUEPILL_F103C8\""
    "HAL_UART_MODULE_ENABLED"     # 只用 UART 的 HAL; CAN/I2C/TIM 走寄存器或 LL
    "USE_HAL_DRIVER"
    "USE_FULL_LL_DRIVER"
    "EXTENDED_PIN_MODE"
    "VARIANT_H=\"variant_PILL_F103Cx.h\""
    "VECT_TAB_OFFSET=0x0"
)

# ---- 5. 编译选项 -------------------------------------------------------------
set(STM32_COMMON_FLAGS
    -Wall
    -Wextra
    -Wno-unused-parameter
    -finput-charset=UTF-8
    -fexec-charset=UTF-8
    -mcpu=cortex-m3
    -mthumb
    -Os
    -ffunction-sections
    -fdata-sections
    -nostdlib
    --param
    max-inline-insns-single=500
)

function(stm32_arduino_apply_flags tgt)
    target_include_directories(${tgt} PUBLIC ${STM32_INCLUDE_DIRS})
    target_compile_definitions(${tgt} PUBLIC ${STM32_DEFINES})
    target_compile_options(${tgt} PUBLIC
        $<$<COMPILE_LANGUAGE:C>:-std=gnu17>
        ${STM32_COMMON_FLAGS}
        $<$<COMPILE_LANGUAGE:CXX>:-std=gnu++17>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-threadsafe-statics>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-rtti>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions>
        $<$<COMPILE_LANGUAGE:CXX>:-fno-use-cxa-atexit>
    )
endfunction()

# ---- 6. 核心库 (cores/arduino) ----------------------------------------------
# ★ 必须包含 *.S: 中断向量表与 Reset_Handler 在
#   cores/arduino/stm32/startup_stm32yyxx.S 里 (同目录的 .c 在 F1 + HAL v1 下
#   编译出来是空的)。漏掉 .S 的症状是链接时报
#   "warning: cannot find entry symbol Reset_Handler", 然后镜像只有几 KB。
#
# ★ 故意不用 file(GLOB ... CONFIGURE_DEPENDS): 它会让 ninja 每次构建都重跑
#   CMake("Re-running CMake"), 进而每次都重新链接 —— 实测白白多花十几秒。
#   本项目自己的源文件都在 CMakeLists 里显式列出, 只有框架源码用 glob,
#   所以加自己的 .c 文件不受影响 (加框架文件才需要手动重新 configure)。
file(GLOB_RECURSE STM32_CORE_SOURCES
    "${STM32DUINO_ROOT}/cores/arduino/*.c"
    "${STM32DUINO_ROOT}/cores/arduino/*.cpp"
    "${STM32DUINO_ROOT}/cores/arduino/*.S"
)
list(LENGTH STM32_CORE_SOURCES _n)
if(_n LESS 20)
    message(FATAL_ERROR
        "cores/arduino 只找到 ${_n} 个源文件, 框架目录可能不完整: ${STM32DUINO_ROOT}")
endif()
unset(_n)

add_library(stm32_core STATIC ${STM32_CORE_SOURCES})
stm32_arduino_apply_flags(stm32_core)
set_target_properties(stm32_core PROPERTIES OUTPUT_NAME "stm32_core")

# ---- 7. SrcWrapper 与 variant (目标文件直接进链接, 与 PIO 一致) --------------
file(GLOB_RECURSE STM32_SRCWRAPPER_SOURCES
    "${STM32DUINO_ROOT}/libraries/SrcWrapper/src/*.c"
    "${STM32DUINO_ROOT}/libraries/SrcWrapper/src/*.cpp"
)
add_library(stm32_srcwrapper OBJECT ${STM32_SRCWRAPPER_SOURCES})
stm32_arduino_apply_flags(stm32_srcwrapper)

file(GLOB STM32_VARIANT_SOURCES
    "${STM32_VARIANT_DIR}/*.c"
    "${STM32_VARIANT_DIR}/*.cpp"
    "${STM32_VARIANT_DIR}/*.S"
)
add_library(stm32_variant OBJECT ${STM32_VARIANT_SOURCES})
stm32_arduino_apply_flags(stm32_variant)

# ---- 8. 固件目标 ------------------------------------------------------------
#
#   stm32_arduino_firmware(SOURCES <应用源文件...> LIBS <库目标...>)
#
function(stm32_arduino_firmware)
    cmake_parse_arguments(FW "" "" "SOURCES;LIBS" ${ARGN})
    set(tgt firmware)

    add_executable(${tgt} ${FW_SOURCES})
    # 裸机镜像固定叫 firmware.elf (CMake 在 Generic 平台上不会自动加后缀)
    set_target_properties(${tgt} PROPERTIES SUFFIX ".elf")
    stm32_arduino_apply_flags(${tgt})

    target_link_libraries(${tgt} PRIVATE
        stm32_variant
        stm32_srcwrapper
        ${FW_LIBS}
        "$<LINK_GROUP:RESCAN,stm32_core,c,m,gcc,stdc++>"
    )

    target_link_options(${tgt} PRIVATE
        -mcpu=cortex-m3
        -mthumb
        -Os
        --specs=nano.specs
        # 通用 STM32 链接脚本 (它内部会 INCLUDE 变体目录里的那份)。
        # 这里用 -Wl,-T,<路径> 而不是 -T"<路径>": 后者会让 CMake 生成 \"...\",
        # 而 ninja 在 Windows 上是通过 cmd.exe 跑的, cmd 不认 \" 转义,
        # ld 最终会去找一个带引号的假文件名。
        -Wl,-T,${STM32DUINO_ROOT}/system/ldscript.ld
        -Wl,--default-script,${STM32_VARIANT_DIR}/ldscript.ld
        -Wl,--gc-sections,--relax
        -Wl,--check-sections
        -Wl,--entry=Reset_Handler
        -Wl,--unresolved-symbols=report-all
        -Wl,--warn-common
        -Wl,--defsym=LD_MAX_SIZE=${STM32_FLASH_SIZE}
        -Wl,--defsym=LD_MAX_DATA_SIZE=${STM32_RAM_SIZE}
        -Wl,--defsym=LD_FLASH_OFFSET=${STM32_FLASH_OFFSET}
        "-Wl,-Map=${CMAKE_CURRENT_BINARY_DIR}/firmware.map"
    )

    # ---- objcopy: ELF -> .bin / .hex ----
    add_custom_command(TARGET ${tgt} POST_BUILD
        BYPRODUCTS
            "${CMAKE_CURRENT_BINARY_DIR}/firmware.bin"
            "${CMAKE_CURRENT_BINARY_DIR}/firmware.hex"
        COMMENT "生成 firmware.bin / firmware.hex"
        COMMAND "${CMAKE_OBJCOPY}" -O binary "$<TARGET_FILE:${tgt}>"
                "${CMAKE_CURRENT_BINARY_DIR}/firmware.bin"
        COMMAND "${CMAKE_OBJCOPY}" -O ihex "$<TARGET_FILE:${tgt}>"
                "${CMAKE_CURRENT_BINARY_DIR}/firmware.hex"
        COMMAND "${CMAKE_SIZE}" --format=berkeley "$<TARGET_FILE:${tgt}>"
        VERBATIM
    )

    # ---- size ----
    add_custom_target(size
        COMMAND "${CMAKE_SIZE}" --format=berkeley "$<TARGET_FILE:${tgt}>"
        DEPENDS ${tgt}
        COMMENT "Flash / RAM 占用"
        VERBATIM
    )

    # ---- flash: 用 OpenOCD + ST-Link 烧录 ----
    set(_openocd "${PIO_PACKAGES_DIR}/tool-openocd/bin/openocd${CMAKE_EXECUTABLE_SUFFIX}")
    set(_ocd_scripts "${PIO_PACKAGES_DIR}/tool-openocd/openocd/scripts")

    add_custom_target(flash
        COMMAND "${_openocd}"
                -s "${_ocd_scripts}"
                -f interface/stlink.cfg
                -f target/stm32f1x.cfg
                -c "program $<TARGET_FILE:${tgt}> verify reset exit"
        DEPENDS ${tgt}
        COMMENT "通过 ST-Link 烧录 firmware.elf"
        USES_TERMINAL
        VERBATIM
    )

    # ---- reset: 只复位, 不重烧 ----
    add_custom_target(reset
        COMMAND "${_openocd}"
                -s "${_ocd_scripts}"
                -f interface/stlink.cfg
                -f target/stm32f1x.cfg
                -c "init; reset run; exit"
        COMMENT "复位目标板"
        USES_TERMINAL
        VERBATIM
    )
endfunction()
