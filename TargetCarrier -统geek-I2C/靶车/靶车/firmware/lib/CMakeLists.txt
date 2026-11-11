# =============================================================================
#  firmware/lib —— 三端共享的库
#
#    protocol/  ESP-NOW → UART → CAN 三级链路的帧格式 (唯一真源)
#    oled/      128x64 OLED 驱动, 硬件无关; 各端自己提供 oled_hal_*.c
#
#  这个文件被三个固件工程 add_subdirectory 进来, 本身不带工具链假设。
#  编译选项由各固件工程在 include 后调用 *_apply_flags(<目标>) 注入,
#  这样三端拿到的宏/优化级别和原来 PlatformIO 构建时一致。
# =============================================================================

# 三端共用的协议库
add_library(protocol STATIC "${CMAKE_CURRENT_SOURCE_DIR}/protocol/protocol.c")
target_include_directories(protocol PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/protocol")
# protocol.c 里有 _Static_assert 长度自检, 任何平台都不许关掉编译告警
set_target_properties(protocol PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    EXCLUDE_FROM_ALL ON)      # 没被链接的工程不编译它

# 三端共用的链路可靠性内核 (序号/过期/ARM 授权/新鲜度/ESP-NOW 统计/CAN 发送协调/成帧)
# 纯 C、无 RTOS 依赖, 可被主机端测试直接编译。
add_library(linkcore STATIC
    "${CMAKE_CURRENT_SOURCE_DIR}/linkcore/lc_guard.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/linkcore/lc_txstats.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/linkcore/lc_can_tx.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/linkcore/lc_frame_rx.c")
target_include_directories(linkcore PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/linkcore/include")
target_link_libraries(linkcore PUBLIC protocol)
set_target_properties(linkcore PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    EXCLUDE_FROM_ALL ON)

# OLED 驱动 (只用到的那端才编译)
add_library(oled STATIC "${CMAKE_CURRENT_SOURCE_DIR}/oled/oled.c")
target_include_directories(oled PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/oled")
set_target_properties(oled PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    EXCLUDE_FROM_ALL ON)

# 家里 WiFi 账号的 NVS 存取 (两块 ESP32 共用, STM32 不链接它)
add_library(netcfg STATIC "${CMAKE_CURRENT_SOURCE_DIR}/netcfg/netcfg.cpp")
target_include_directories(netcfg PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/netcfg")
set_target_properties(netcfg PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
    EXCLUDE_FROM_ALL ON)
