# =============================================================================
#  toolchain-esp32s3.cmake —— ESP32-S3 (Xtensa LX7) 交叉编译工具链
#
#  不下载任何东西, 直接复用 PlatformIO 已安装的
#      <packages>/toolchain-xtensa-esp32s3/bin/xtensa-esp32s3-elf-*.exe
#
#  用法 (通常由 CMakePresets.json 自动带入, 不用手敲):
#      cmake -S firmware/esp32_master -B build \
#            -DCMAKE_TOOLCHAIN_FILE=../../cmake/toolchain-esp32s3.cmake
# =============================================================================

set(CMAKE_SYSTEM_NAME      Generic)
set(CMAKE_SYSTEM_PROCESSOR xtensa)

# packages 目录由 pio_sdk.cmake 统一解析 (支持 -DPIO_PACKAGES_DIR / 环境变量)
include("${CMAKE_CURRENT_LIST_DIR}/pio_sdk.cmake")
pio_require_package(toolchain-xtensa-esp32s3)

# 宿主机可执行文件后缀。
# 注意: 这里不能用 CMAKE_EXECUTABLE_SUFFIX —— 本文件末尾会把它改成 .elf,
# 而工具链文件在 project() 时可能被重复求值, 那样就会拼出 "gcc.elf" 这种错路径。
if(WIN32)
    set(_host_exe ".exe")
else()
    set(_host_exe "")
endif()

set(ESP32S3_TOOLCHAIN_DIR "${PIO_PACKAGES_DIR}/toolchain-xtensa-esp32s3")
set(ESP32S3_TOOLCHAIN_BIN "${ESP32S3_TOOLCHAIN_DIR}/bin")
set(_triple "xtensa-esp32s3-elf")

set(CMAKE_C_COMPILER   "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-gcc${_host_exe}")
set(CMAKE_CXX_COMPILER "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-g++${_host_exe}")
set(CMAKE_ASM_COMPILER "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-gcc${_host_exe}")
set(CMAKE_AR           "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-ar${_host_exe}")
set(CMAKE_RANLIB       "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-ranlib${_host_exe}")
set(CMAKE_OBJCOPY      "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-objcopy${_host_exe}")
set(CMAKE_OBJDUMP      "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-objdump${_host_exe}")
set(CMAKE_SIZE         "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-size${_host_exe}")
set(CMAKE_GDB          "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-gdb${_host_exe}")
set(CMAKE_STRIP        "${ESP32S3_TOOLCHAIN_BIN}/${_triple}-strip${_host_exe}")

# 裸机目标: 编译器探针只需要编出目标文件, 不需要链接出可执行文件
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

# 只在这个前缀下搜索库/头文件, 免得误用宿主机的库
set(CMAKE_FIND_ROOT_PATH "${ESP32S3_TOOLCHAIN_DIR}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# 产物是裸机镜像, 统一用 .elf 后缀 (和 PlatformIO 一致)
set(CMAKE_EXECUTABLE_SUFFIX ".elf")

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
