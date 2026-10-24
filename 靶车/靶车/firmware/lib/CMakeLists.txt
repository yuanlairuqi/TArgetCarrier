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

# OLED 驱动 (只用到的那端才编译)
add_library(oled STATIC "${CMAKE_CURRENT_SOURCE_DIR}/oled/oled.c")
target_include_directories(oled PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/oled")
set_target_properties(oled PROPERTIES
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
    EXCLUDE_FROM_ALL ON)
