/**
 * @file    lc_frame_rx.c
 * @brief   字节流成帧器 —— UART/DMA 消费侧的公共实现
 *
 *  设计动机:
 *    - STM32 用 DMA 环形缓冲收串口, 任务消费积压字节时刻可能滞后;
 *    - 桥接端用 Serial1 轮询, 同样可能在一帧中间插入其它帧类型的字节;
 *    - 旧实现每收满 17 字节就解析一次, CRC 失败后只丢整段, 容易把紧随其后的
 *      好帧一起丢掉; 也没有"这帧已经在缓冲里躺了多久"的判断。
 *  本模块:
 *    - 校验失败时在已收字节里重新找帧头, 不连带丢掉下一帧;
 *    - 记录最后有效帧时间与统计, 由调用方按有效期丢弃过期命令;
 *    - 只做成帧/校验, 不解析业务字段 (业务解析仍用 protocol.h 的 decode)。
 */

#include "linkcore.h"

#include <string.h>

static bool is_header(uint8_t b)
{
    return b == PROTO_HEAD || b == PROTO_STX;
}

void lc_rx_init(lc_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    memset(rx, 0, sizeof(*rx));
}

void lc_rx_overflow(lc_rx_t *rx)
{
    if (rx == NULL) {
        return;
    }
    rx->len = 0;
    rx->overflow++;
}

/* 把 buf 中 [from, len) 的内容前移, 丢掉 from 之前的字节 */
static void rx_slide(lc_rx_t *rx, uint8_t from)
{
    if (from == 0u || from >= rx->len) {
        return;
    }
    uint8_t n = (uint8_t)(rx->len - from);
    memmove(rx->buf, rx->buf + from, n);
    rx->len = n;
    rx->resync++;
}

static lc_rx_kind_t classify(const uint8_t *f)
{
    if (f[0] == PROTO_STX && f[2] == (uint8_t)PROTO_MSG_STATUS) {
        return LC_RX_STATUS;
    }
    if (f[0] == PROTO_HEAD) {
        switch (f[2]) {
        case (uint8_t)PROTO_MSG_CTRL:         return LC_RX_CTRL;
        case (uint8_t)PROTO_MSG_GAINS:        return LC_RX_GAINS;
        case (uint8_t)PROTO_MSG_PARAM:        return LC_RX_PARAM;
        case (uint8_t)PROTO_MSG_PARAM_REPLY:  return LC_RX_PARAM_REPLY;
        case (uint8_t)PROTO_MSG_MOTOR:        return LC_RX_MOTOR;
        case (uint8_t)PROTO_MSG_BRIDGE_STATUS:return LC_RX_BRIDGE_STATUS;
        default:                              return LC_RX_UNKNOWN;
        }
    }
    return LC_RX_UNKNOWN;
}

static bool validate(const uint8_t *f)
{
    switch (classify(f)) {
    case LC_RX_CTRL: {
        ctrl_payload_t pl;
        return proto_decode_ctrl(f, CTRL_FRAME_SIZE, &pl, NULL);
    }
    case LC_RX_GAINS: {
        float kp, kd;
        return proto_decode_gains(f, GAINS_FRAME_SIZE, &kp, &kd);
    }
    case LC_RX_PARAM: {
        uint8_t op, rid, v[4];
        return proto_decode_param(f, CTRL_FRAME_SIZE, &op, &rid, v);
    }
    case LC_RX_STATUS: {
        status_frame_t st;
        return proto_decode_status(f, STATUS_FRAME_SIZE, &st);
    }
    case LC_RX_MOTOR: {
        return proto_decode_motor(f, CTRL_FRAME_SIZE, NULL, NULL, NULL, NULL);
    }
    case LC_RX_PARAM_REPLY: {
        /* 回包与参数帧同壳, 用同一套 CRC/帧尾规则校验 */
        if (f[1] != 13u || f[16] != PROTO_TAIL) {
            return false;
        }
        return f[15] == (uint8_t)(proto_crc16(&f[1], 14) & 0xFFu);
    }
    case LC_RX_BRIDGE_STATUS: {
        bridge_status_frame_t bst;
        return proto_decode_bridge_status(f, BRIDGE_STATUS_FRAME_SIZE, &bst);
    }
    default:
        return false;
    }
}

lc_rx_kind_t lc_rx_feed(lc_rx_t *rx, uint8_t byte, uint32_t now_ms, uint8_t out[17])
{
    if (rx == NULL) {
        return LC_RX_NONE;
    }

    /* 找帧头阶段: 非帧头字节直接丢, 天然重同步 */
    if (rx->len == 0u) {
        if (!is_header(byte)) {
            return LC_RX_NONE;
        }
        rx->buf[0] = byte;
        rx->len = 1;
        return LC_RX_NONE;
    }

    rx->buf[rx->len++] = byte;
    if (rx->len < 17u) {
        return LC_RX_NONE;
    }

    /* 收满 17 字节: 校验 */
    if (validate(rx->buf)) {
        lc_rx_kind_t kind = classify(rx->buf);
        if (out != NULL) {
            memcpy(out, rx->buf, 17u);
        }
        rx->len = 0;
        rx->frames_ok++;
        rx->last_valid_ms = now_ms;
        rx->have_valid = true;
        return kind;
    }

    rx->frames_bad++;
    /* 区分 CRC/帧尾失败与其它失败, 便于现场判断是干扰还是协议不匹配 */
    if (rx->buf[0] == PROTO_HEAD) {
        if (rx->buf[16] != PROTO_TAIL ||
            rx->buf[15] != (uint8_t)(proto_crc16(&rx->buf[1], 14) & 0xFFu)) {
            rx->crc_bad++;
        }
    } else {
        rx->crc_bad++;
    }

    /* 失败后不能整段丢弃: 缓冲里(1..16)可能还有下一帧的帧头。
     * 从第 1 字节开始找, 找到就前移继续收; 找不到就清空重新同步。 */
    for (uint8_t i = 1u; i < 17u; i++) {
        if (is_header(rx->buf[i])) {
            rx_slide(rx, i);
            return LC_RX_NONE;
        }
    }
    rx->len = 0;
    return LC_RX_NONE;
}
