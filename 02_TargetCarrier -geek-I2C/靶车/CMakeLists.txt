# =============================================================================
#  靶车 —— 顶层 CMake 工程
#
#  这一个文件干两件事:
#    1. 用**宿主编译器**编译协议库自测程序 test_protocol (不需要硬件, 直接跑)
#    2. 用 ExternalProject 把三个交叉固件工程串起来:
#         firmware/esp32_master  -> ESP32-S3  (Xtensa 工具链)
#         firmware/esp32_bridge  -> ESP32-S3  (Xtensa 工具链)
#         firmware/stm32_f103    -> STM32F103 (ARM 工具链)
#
#  ★ 为什么顶层要串三个独立工程, 而不是 add_subdirectory?
#    因为三块板需要**三套不同的交叉工具链**, 而一个 CMake 构建树只能有一
#    套工具链。所以每个固件都是独立的 CMake 工程 (自己的 CMakeLists.txt +
#    自己的 CMakePresets.json), 顶层只负责按顺序调用它们。
#
#  用法 (VS Code):
#      用 VS Code 打开仓库根目录 -> 选预设 "all" -> Build
#  用法 (命令行):
#      cmake --preset all
#      cmake --build --preset all                 # 编三个固件 + 协议自测
#      ctest --preset all                         # 跑主机端协议自测
#
#  ★ 单独编译/烧录某一块板, 请直接打开那个固件文件夹 (推荐, 见各目录 README):
#      firmware/esp32_master  -> 预设 esp32-master
#      firmware/esp32_bridge  -> 预设 esp32-bridge
#      firmware/stm32_f103    -> 预设 stm32-f103
# =============================================================================

cmake_minimum_required(VERSION 3.25)

project(target_vehicle LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED OFF)

enable_testing()

# =============================================================================
#  1. 主机端: 协议库自测 (不接任何硬件)
# =============================================================================
set(PROTOCOL_DIR "${CMAKE_CURRENT_SOURCE_DIR}/firmware/lib/protocol")

add_executable(test_protocol "${PROTOCOL_DIR}/test/test_protocol.c")
target_include_directories(test_protocol PRIVATE "${PROTOCOL_DIR}")

# MSVC 下源码是 UTF-8 (文件里有中文), 不加 /utf-8 会按本地代码页解释, 中文注释
# 里的字节可能被当成转义序列。GCC/Clang 默认就是 UTF-8, 无需处理。
if(MSVC)
    target_compile_options(test_protocol PRIVATE /utf-8 /W4)
    # MSVC 的 C 前端忽略 #pragma pack, 但协议库已把线格式与结构体布局解耦,
    # 所以这里不需要额外操作 —— test_protocol.c 里有字节级断言兜底。
else()
    target_compile_options(test_protocol PRIVATE -Wall -Wextra)
    target_link_libraries(test_protocol PRIVATE m)
endif()

add_test(NAME protocol_selftest COMMAND test_protocol)

# ---- 主机端: WebSocket 协议工具自测 (SHA-1 / Base64 / 握手 / 帧) ----
set(WS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/firmware/esp32_master")
add_executable(test_ws_proto
    "${WS_DIR}/test/ws_proto_test.c"
    "${WS_DIR}/src/ws_proto.c")
target_include_directories(test_ws_proto PRIVATE "${WS_DIR}/src")
if(MSVC)
    target_compile_options(test_ws_proto PRIVATE /utf-8 /W4)
else()
    target_compile_options(test_ws_proto PRIVATE -Wall -Wextra)
endif()
add_test(NAME ws_proto_selftest COMMAND test_ws_proto)

# ---- 主机端: 链路可靠性内核自测 (序号/过期/ARM/新鲜度/统计/CAN 协调/成帧) ----
set(LINKCORE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/firmware/lib/linkcore")
add_executable(test_linkcore
    "${LINKCORE_DIR}/test/test_linkcore.c"
    "${LINKCORE_DIR}/lc_guard.c"
    "${LINKCORE_DIR}/lc_txstats.c"
    "${LINKCORE_DIR}/lc_can_tx.c"
    "${LINKCORE_DIR}/lc_frame_rx.c"
    "${PROTOCOL_DIR}/protocol.c")
target_include_directories(test_linkcore PRIVATE
    "${LINKCORE_DIR}/include"
    "${PROTOCOL_DIR}")
if(MSVC)
    target_compile_options(test_linkcore PRIVATE /utf-8 /W4)
else()
    target_compile_options(test_linkcore PRIVATE -Wall -Wextra)
    target_link_libraries(test_linkcore PRIVATE m)
endif()
add_test(NAME linkcore_selftest COMMAND test_linkcore)

# Host register doubles exercise the real CAN driver, including result collection.
add_executable(test_can_driver
    firmware/stm32_f103/test/test_can_driver.c
    firmware/stm32_f103/src/can_drv.c)
target_include_directories(test_can_driver PRIVATE
    firmware/stm32_f103/test/mocks firmware/stm32_f103/include)
if(MSVC)
    target_compile_options(test_can_driver PRIVATE /utf-8 /W4)
else()
    target_compile_options(test_can_driver PRIVATE -Wall -Wextra)
endif()
add_test(NAME can_driver_regression COMMAND test_can_driver)


# =============================================================================
#  2. 三个交叉固件
# =============================================================================
include(ExternalProject)

# STM32 的构建目录必须是纯 ASCII (arm-none-eabi-ld 在非 ASCII 路径下写不出
# map 文件)。想改位置就传 -DVEHICLE_STM32_BUILD_DIR=...
if(WIN32)
    set(_default_stm32_dir "C:/cmake_build/target-vehicle/stm32")
else()
    set(_default_stm32_dir "${CMAKE_BINARY_DIR}/stm32")
endif()
set(VEHICLE_STM32_BUILD_DIR "${_default_stm32_dir}" CACHE PATH
    "STM32 固件的构建目录 (必须是纯 ASCII 路径)")
unset(_default_stm32_dir)

# vehicle_firmware(<名字> <工具链文件> <构建目录> [额外的 -D...])
function(vehicle_firmware name toolchain binary_dir)
    set(_extra ${ARGN})
    ExternalProject_Add(${name}
        SOURCE_DIR   "${CMAKE_CURRENT_SOURCE_DIR}/firmware/${name}"
        BINARY_DIR   "${binary_dir}"
        # 交叉编译必须用单配置的 Ninja, 不能继承顶层的多配置生成器
        CMAKE_GENERATOR Ninja
        CMAKE_ARGS
            "-DCMAKE_TOOLCHAIN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/cmake/${toolchain}"
            "-DCMAKE_BUILD_TYPE=Release"
            ${_extra}
        UPDATE_COMMAND ""          # 没有版本库远端, 不需要 update 步骤
        INSTALL_COMMAND ""
        TEST_COMMAND ""
        BUILD_ALWAYS TRUE
        USES_TERMINAL_BUILD TRUE
    )
endfunction()

vehicle_firmware(esp32_master toolchain-esp32s3.cmake
                 "${CMAKE_BINARY_DIR}/esp32_master")
vehicle_firmware(esp32_bridge toolchain-esp32s3.cmake
                 "${CMAKE_BINARY_DIR}/esp32_bridge")
vehicle_firmware(stm32_f103   toolchain-stm32f103.cmake
                 "${VEHICLE_STM32_BUILD_DIR}")

# 方便: 一条命令把三个固件的 .bin 位置打出来
add_custom_target(firmware-info
    COMMAND ${CMAKE_COMMAND} -E echo "----------------------------------------------------------"
    COMMAND ${CMAKE_COMMAND} -E echo "ESP32 主机   : ${CMAKE_BINARY_DIR}/esp32_master/firmware.bin"
    COMMAND ${CMAKE_COMMAND} -E echo "ESP32 桥接   : ${CMAKE_BINARY_DIR}/esp32_bridge/firmware.bin"
    COMMAND ${CMAKE_COMMAND} -E echo "STM32 控制器 : ${VEHICLE_STM32_BUILD_DIR}/firmware.bin"
    COMMAND ${CMAKE_COMMAND} -E echo "----------------------------------------------------------"
    VERBATIM
)

message(STATUS "----------------------------------------------------------")
message(STATUS " 靶车 · 顶层工程")
message(STATUS "   协议自测   : test_protocol  (ctest 跑)")
message(STATUS "   三个固件   : esp32_master / esp32_bridge / stm32_f103")
message(STATUS "   STM32 构建 : ${VEHICLE_STM32_BUILD_DIR}")
message(STATUS "----------------------------------------------------------")
