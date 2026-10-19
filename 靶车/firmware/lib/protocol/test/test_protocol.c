/**
 * @file    test_protocol.c
 * @brief   协议库主机端自测 —— 不依赖任何单片机
 *
 *  用 CMake 跑 (推荐, 在仓库根目录):
 *      cmake --preset all
 *      ctest --preset all
 *  或者只编这一个目标再手动运行:
 *      cmake --build --preset all --target test_protocol
 *      ./build/Debug/test_protocol.exe        # Windows (Visual Studio 生成器)
 *      ./build/test_protocol                  # 单配置生成器
 *  或在任意有 C 编译器的地方:
 *      gcc -I. test/test_protocol.c -o t -lm && ./t
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "protocol.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            printf("  [PASS] %s\n", msg);                           \
        } else {                                                    \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);      \
            g_fail++;                                               \
        }                                                           \
    } while (0)

static void test_layout(void)
{
    printf("\n== 1. 线格式长度 ==\n");
    CHECK(CTRL_FRAME_SIZE == 17u, "控制帧 17 字节");
    CHECK(RX_FRAME_SIZE == 17u,   "反馈帧 17 字节");
    /* 注意: 这里绝不能断言 sizeof(ctrl_payload_t) == 9。
     * MSVC 的 C 前端会忽略 #pragma pack, 自然对齐下是 11 字节, 而 GCC 是 9。
     * 线格式与结构体布局解耦, 所以只查逻辑长度。 */
    CHECK(CTRL_PAYLOAD_LOGIC_SIZE == 9u, "控制载荷逻辑长度 9 字节");
}

static void test_roundtrip(void)
{
    printf("\n== 2. 编码->解码 往返一致性 ==\n");

    ctrl_target_t in = { 12.34f, -45.67f, 88.0f, -12.5f };
    uint8_t buf[CTRL_FRAME_SIZE];
    ctrl_payload_t pl;
    ctrl_target_t  out;
    uint8_t seq = 0;

    proto_encode_ctrl(buf, &in, CTRL_FLAG_ENABLE, MOTOR_MODE_MIT, 7);

    CHECK(buf[0] == PROTO_HEAD, "帧头 0xAA");
    CHECK(buf[1] == 13u,        "len 字段 = 13");
    CHECK(buf[2] == PROTO_MSG_CTRL, "type = CTRL");
    CHECK(buf[3] == 7u,         "seq 透传");
    CHECK(buf[16] == PROTO_TAIL, "帧尾 0x55");
    CHECK(buf[15] == (uint8_t)(proto_crc16(&buf[1], 14) & 0xFF), "CRC 字段正确");

    CHECK(proto_decode_ctrl(buf, sizeof(buf), &pl, &seq), "解码成功");
    CHECK(seq == 7u, "解码 seq");
    CHECK(pl.flags == CTRL_FLAG_ENABLE, "解码 flags");
    CHECK(pl.mode == MOTOR_MODE_MIT, "解码 mode");

    proto_payload_to_target(&pl, &out);
    /* 量化步长 0.01 度, 允许半个步长误差 */
    CHECK(fabsf(out.yaw_deg - in.yaw_deg) <= 0.01f, "yaw 往返误差 <= 0.01");
    CHECK(fabsf(out.pitch_deg - in.pitch_deg) <= 0.01f, "pitch 往返误差 <= 0.01");
    CHECK(fabsf(out.yaw_rate_dps - in.yaw_rate_dps) <= 0.1f, "yaw rate 往返误差 <= 0.1");
    CHECK(fabsf(out.pitch_rate_dps - in.pitch_rate_dps) <= 0.1f, "pitch rate 往返误差 <= 0.1");
}

static void test_limits(void)
{
    printf("\n== 3. 超量程限幅 ==\n");

    /* 故意给超量程的值, 编码后必须被夹到协议量程内, 不能回绕成反向大角度 */
    ctrl_target_t in = { 9999.0f, -9999.0f, 9999.0f, -9999.0f };
    uint8_t buf[CTRL_FRAME_SIZE];
    ctrl_payload_t pl;
    ctrl_target_t  out;

    proto_encode_ctrl(buf, &in, 0, MOTOR_MODE_MIT, 0);
    CHECK(proto_decode_ctrl(buf, sizeof(buf), &pl, NULL), "超量程帧仍可解码");
    proto_payload_to_target(&pl, &out);

    CHECK(fabsf(out.yaw_deg - PROTO_YAW_LIMIT_DEG) <= 0.01f, "yaw 夹到 +180");
    CHECK(fabsf(out.pitch_deg + PROTO_PITCH_LIMIT_DEG) <= 0.01f, "pitch 夹到 -90");
    CHECK(fabsf(out.yaw_rate_dps - PROTO_RATE_LIMIT_DPS) <= 0.1f, "yaw rate 夹到 +180");
}

static void test_reject(void)
{
    printf("\n== 4. 坏帧必须被拒绝 ==\n");

    ctrl_target_t in = { 1.0f, 2.0f, 3.0f, 4.0f };
    uint8_t buf[CTRL_FRAME_SIZE];
    ctrl_payload_t pl;
    uint8_t bad[CTRL_FRAME_SIZE];

    proto_encode_ctrl(buf, &in, 0, MOTOR_MODE_MIT, 1);

    CHECK(!proto_decode_ctrl(buf, CTRL_FRAME_SIZE - 1, &pl, NULL), "截断帧被拒");
    CHECK(!proto_decode_ctrl(buf, 0, &pl, NULL), "空帧被拒");

    memcpy(bad, buf, sizeof(bad)); bad[0] = 0xAB;
    CHECK(!proto_decode_ctrl(bad, sizeof(bad), &pl, NULL), "错误帧头被拒");

    memcpy(bad, buf, sizeof(bad)); bad[16] = 0x00;
    CHECK(!proto_decode_ctrl(bad, sizeof(bad), &pl, NULL), "错误帧尾被拒");

    memcpy(bad, buf, sizeof(bad)); bad[9] ^= 0x01;
    CHECK(!proto_decode_ctrl(bad, sizeof(bad), &pl, NULL), "数据位翻转被 CRC 抓到");

    memcpy(bad, buf, sizeof(bad)); bad[2] = PROTO_MSG_STATUS;
    CHECK(!proto_decode_ctrl(bad, sizeof(bad), &pl, NULL), "错误 type 被拒");
}

static void test_crc_known(void)
{
    printf("\n== 5. CRC16-Modbus 已知向量 ==\n");
    /* "123456789" 的 CRC16/MODBUS 标准结果 = 0x4B37 */
    const uint8_t v[] = "123456789";
    uint16_t crc = proto_crc16(v, 9);
    printf("  CRC16(\"123456789\") = 0x%04X (期望 0x4B37)\n", crc);
    CHECK(crc == 0x4B37u, "标准向量匹配");

    CHECK(proto_crc16((const uint8_t *)"", 0) == 0xFFFFu, "空数据初值 0xFFFF");
}

static void test_status_frame(void)
{
    printf("\n== 6. 反馈帧编码/解码 ==\n");

    uint8_t buf[RX_FRAME_SIZE];
    status_frame_t st;

    proto_encode_status(buf, 42, 55, LINK_STATE_OK, -33.25f, 7.75f, 46, 0);

    CHECK(buf[0] == PROTO_STX, "反馈帧头 0x5A");
    CHECK(buf[1] == 15u, "len 字段 = 15");
    CHECK(buf[14] == 46u, "temp_mos 落在字节 14");
    CHECK(buf[15] == 0u, "err_code 落在字节 15");
    CHECK(buf[16] == (uint8_t)(proto_crc16(&buf[1], 15) & 0xFF), "反馈 CRC 落在字节 16");

    CHECK(proto_decode_status(buf, sizeof(buf), &st), "反馈解码成功");
    CHECK(st.seq == 42u && st.rssi == 55u, "seq/rssi");
    CHECK(st.state == LINK_STATE_OK, "state");
    CHECK(fabsf(st.yaw_deg + 33.25f) < 1e-6f, "yaw 浮点无损");
    CHECK(fabsf(st.pitch_deg - 7.75f) < 1e-6f, "pitch 浮点无损");
    CHECK(st.temp_mos == 46u && st.err_code == 0u, "温度/错误码");

    buf[10] ^= 0x80;
    CHECK(!proto_decode_status(buf, sizeof(buf), &st), "反馈帧被篡改 -> 拒绝");

    /* 回归保护: 线格式是唯一真源, 任何一侧改布局这里必须红 */
    {
        uint8_t ref[RX_FRAME_SIZE];
        proto_encode_status(ref, 1, 2, 3, 1.0f, 2.0f, 3, 4);
        CHECK(ref[0] == 0x5Au && ref[1] == 15u && ref[2] == 0x02u, "反馈帧前 3 字节固定布局");
        CHECK(ref[3] == 1u && ref[4] == 2u && ref[5] == 3u, "seq/rssi/state 位置固定");
        CHECK(ref[14] == 3u && ref[15] == 4u, "temp/err 位置固定");
    }
}

static void test_all_modes(void)
{
    printf("\n== 7. 四种电机模式编解码 ==\n");
    ctrl_target_t in = { 0.0f, 0.0f, 0.0f, 0.0f };
    uint8_t buf[CTRL_FRAME_SIZE];
    ctrl_payload_t pl;

    for (uint8_t m = 0; m < 4; m++) {
        proto_encode_ctrl(buf, &in, CTRL_FLAG_ENABLE, m, m);
        CHECK(proto_decode_ctrl(buf, sizeof(buf), &pl, NULL) && pl.mode == m,
              "模式字段往返");
    }

    /* 所有标志位组合都要能过 */
    uint16_t f = CTRL_FLAG_ENABLE | CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO | CTRL_FLAG_ESTOP;
    proto_encode_ctrl(buf, &in, f, MOTOR_MODE_MIT, 0);
    CHECK(proto_decode_ctrl(buf, sizeof(buf), &pl, NULL) && pl.flags == f, "标志位全开往返");
}

int main(void)
{
    printf("========================================\n");
    printf("  靶车通信协议 自测\n");
    printf("========================================\n");

    test_layout();
    test_roundtrip();
    test_limits();
    test_reject();
    test_crc_known();
    test_status_frame();
    test_all_modes();

    printf("\n========================================\n");
    if (g_fail == 0) {
        printf("  全部通过 ✓\n");
    } else {
        printf("  失败 %d 项 ✗\n", g_fail);
    }
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
