# =============================================================================
#  toolchain-stm32f103.cmake —— STM32F103C8T6 (ARM Cortex-M3) 交叉编译工具链
#
#  复用 PlatformIO 已安装的
#      <packages>/toolchain-gccarmnoneeabi/bin/arm-none-eabi-*.exe
#
#  用法 (通常由 CMakePresets.json 自动带入, 不用手敲):
#      cmake -S firmware/stm32_f103 -B <纯 ASCII 构建目录> \
#            -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-stm32f103.cmake
#
#  ★ 注意: 构建目录必须放在纯 ASCII 路径下。
#    这不是洁癖, 是工具链的硬限制 —— 详见 firmware/stm32_f103/CMakeLists.txt 顶部。
# =============================================================================

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

include("${CMAKE_CURRENT_LIST_DIR}/pio_sdk.cmake")
pio_require_package(toolchain-gccarmnoneeabi)

# 宿主机可执行文件后缀 (不能用 CMAKE_EXECUTABLE_SUFFIX, 见 esp32s3 工具链里的说明)
if(WIN32)
    set(_host_exe ".exe")
else()
    set(_host_exe "")
endif()

set(STM32_TOOLCHAIN_DIR "${PIO_PACKAGES_DIR}/toolchain-gccarmnoneeabi")
set(STM32_TOOLCHAIN_BIN "${STM32_TOOLCHAIN_DIR}/bin")
set(_triple "arm-none-eabi")

set(CMAKE_C_COMPILER   "${STM32_TOOLCHAIN_BIN}/${_triple}-gcc${_host_exe}")
set(CMAKE_CXX_COMPILER "${STM32_TOOLCHAIN_BIN}/${_triple}-g++${_host_exe}")
set(CMAKE_ASM_COMPILER "${STM32_TOOLCHAIN_BIN}/${_triple}-gcc${_host_exe}")
set(CMAKE_AR           "${STM32_TOOLCHAIN_BIN}/${_triple}-ar${_host_exe}")
set(CMAKE_RANLIB       "${STM32_TOOLCHAIN_BIN}/${_triple}-ranlib${_host_exe}")
set(CMAKE_OBJCOPY      "${STM32_TOOLCHAIN_BIN}/${_triple}-objcopy${_host_exe}")
set(CMAKE_OBJDUMP      "${STM32_TOOLCHAIN_BIN}/${_triple}-objdump${_host_exe}")
set(CMAKE_SIZE         "${STM32_TOOLCHAIN_BIN}/${_triple}-size${_host_exe}")
set(CMAKE_GDB          "${STM32_TOOLCHAIN_BIN}/${_triple}-gdb${_host_exe}")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_FIND_ROOT_PATH "${STM32_TOOLCHAIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(CMAKE_EXECUTABLE_SUFFIX ".elf")


# Cortex-M3 目标机选项 (编译与链接都要用)
set(STM32_MCU_FLAGS "-mcpu=cortex-m3" "-mthumb")
# GNU ld 的 "重复扫描" 语义 (--start-group / --end-group)。
# CMAKE_SYSTEM_NAME=Generic 时 CMake 不会自动认识它, 需要按语言显式定义,
# 否则 $<LINK_GROUP:RESCAN,...> 会报 "not defined for the CXX link language"。
# 变量名请勿改成不带语言前缀的形式 —— CMake 认的是 CMAKE_<LANG>_LINK_GROUP_USING_*。
set(CMAKE_C_LINK_GROUP_USING_RESCAN "-Wl,--start-group" "-Wl,--end-group")
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN "-Wl,--start-group" "-Wl,--end-group")
set(CMAKE_ASM_LINK_GROUP_USING_RESCAN "-Wl,--start-group" "-Wl,--end-group")
set(CMAKE_C_LINK_GROUP_USING_RESCAN_SUPPORTED   TRUE)
set(CMAKE_CXX_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
set(CMAKE_ASM_LINK_GROUP_USING_RESCAN_SUPPORTED TRUE)
