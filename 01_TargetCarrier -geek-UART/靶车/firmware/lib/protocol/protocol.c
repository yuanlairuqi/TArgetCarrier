/**
 * @file    protocol.c
 * @brief   协议库锚点
 *
 *  协议本身是纯头文件 (protocol.h)，三个工程各自 include 同一份。
 *  这个 .c 存在的唯一目的是让 PlatformIO 的 Library Dependency Finder
 *  把 lib/protocol 识别成一个库；同时把编译期断言放在这里，
 *  一旦有人改了帧长度，三个工程会在编译期就报错，而不是等到现场跑不通。
 */

#include "protocol.h"

/* 线格式长度自检: 改协议时必须同步改这里 */
_Static_assert(CTRL_FRAME_SIZE == 17u,   "控制帧长度变了, 三端必须同步修改!");
_Static_assert(STATUS_FRAME_SIZE == 17u, "反馈帧长度变了, 三端必须同步修改!");
_Static_assert(BRIDGE_STATUS_FRAME_SIZE == 17u, "桥接新鲜度帧必须是 17 字节同壳!");
_Static_assert(CTRL_PAYLOAD_LOGIC_SIZE == 9u, "控制载荷逻辑长度变了!");
/* status_frame_t 含有 float, 布局依编译器而异, 它不是线格式, 只做下限检查 */
_Static_assert(sizeof(status_frame_t) >= 16u, "反馈结构体异常");
/* v1.1 新增 flag 位不能与旧位冲突 */
_Static_assert((CTRL_FLAG_ARM & (CTRL_FLAG_ENABLE | CTRL_FLAG_CLEAR_ERR |
                                 CTRL_FLAG_SET_ZERO | CTRL_FLAG_ESTOP)) == 0u,
               "ARM 标志位与旧 flags 冲突!");
_Static_assert((CTRL_FLAG_FAILSAFE & (CTRL_FLAG_ENABLE | CTRL_FLAG_ARM)) == 0u,
               "FAILSAFE 位不能与 ENABLE/ARM 混用!");

/* 防止空编译单元告警 */
const char *protocol_lib_version(void);
const char *protocol_lib_version(void)
{
    return "target-vehicle-protocol/1.1";
}
