/**
 * @file    ws_proto_test.c
 * @brief   WebSocket 工具函数的主机端单元测试 (不需要板子)
 *
 *  验证三块最容易写错的地方:
 *    1. SHA-1 / Base64 —— 用标准测试向量
 *    2. 握手应答 —— 用 RFC 6455 里的示例 key
 *    3. 帧编解码 —— 自己编码再自己解析 (含掩码)
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ws_proto.h"

static int g_fail = 0;

#define CHECK(cond, msg) do {                                     \
    if (!(cond)) {                                                \
        printf("FAIL: %s  (%s:%d)\n", msg, __FILE__, __LINE__);   \
        g_fail++;                                                 \
    }                                                             \
} while (0)

static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0x0F];
    }
    out[n * 2] = '\0';
}

int main(void)
{
    /* ---- 1. SHA-1 标准向量 ---- */
    {
        uint8_t d[20];
        char hex[41];

        ws_sha1((const uint8_t *)"abc", 3, d);
        to_hex(d, 20, hex);
        CHECK(strcmp(hex, "a9993e364706816aba3e25717850c26c9cd0d89d") == 0, "sha1(abc)");

        ws_sha1((const uint8_t *)"", 0, d);
        to_hex(d, 20, hex);
        CHECK(strcmp(hex, "da39a3ee5e6b4b0d3255bfef95601890afd80709") == 0, "sha1(\"\")");

        /* 超过一个块的输入 (RFC 6455 示例里的完整字符串) */
        const char *longmsg =
            "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        ws_sha1((const uint8_t *)longmsg, strlen(longmsg), d);
        char b64[32];
        size_t n = ws_base64(d, 20, b64, sizeof(b64));
        CHECK(n == 28, "base64 输出长度 28");
        CHECK(strcmp(b64, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "RFC 6455 握手应答");
    }

    /* ---- 2. 握手应答 ---- */
    {
        char acc[29];
        CHECK(ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==", acc), "accept_key 成功");
        CHECK(strcmp(acc, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") == 0, "accept_key 值正确");
    }

    /* ---- 3. 帧编码/解码 ---- */
    {
        uint8_t frame[256];
        const char *msg = "{\"yaw\":1.23}";     /* 12 字节, 短帧路径 */
        size_t n = ws_encode_text(msg, strlen(msg), frame, sizeof(frame));
        CHECK(n == 2 + strlen(msg), "短帧总长度");

        uint8_t opcode;
        bool masked;
        size_t plen, hlen;
        int hr = ws_parse_header(frame, n, &opcode, &masked, &plen, &hlen);
        CHECK(hr == 2, "短帧头部长度");
        CHECK(opcode == WS_OP_TEXT, "opcode = text");
        CHECK(masked == false, "服务端帧不掩码");
        CHECK(plen == strlen(msg), "载荷长度");
        CHECK(memcmp(frame + hlen, msg, plen) == 0, "载荷内容");
    }

    {
        /* 126 字节的长帧路径 (扩展长度) */
        char big[200];
        memset(big, 'A', sizeof(big));
        big[sizeof(big) - 1] = '\0';

        uint8_t frame[400];
        size_t n = ws_encode_text(big, 150, frame, sizeof(frame));
        CHECK(n == 4 + 150, "长帧总长度");

        uint8_t opcode;
        bool masked;
        size_t plen, hlen;
        int hr = ws_parse_header(frame, n, &opcode, &masked, &plen, &hlen);
        CHECK(hr == 4, "长帧头部长度");
        CHECK(plen == 150, "长帧载荷长度");
    }

    {
        /* 客户端帧 (带掩码) 的解码 */
        const char *payload = "k 45";
        uint8_t mask[4] = { 0x12, 0x34, 0x56, 0x78 };
        uint8_t frame[64];
        size_t o = 0;
        frame[o++] = 0x80u | WS_OP_TEXT;
        frame[o++] = (uint8_t)(0x80u | strlen(payload));   /* MASK=1 */
        memcpy(frame + o, mask, 4); o += 4;
        memcpy(frame + o, payload, strlen(payload));
        ws_apply_mask(frame + o, strlen(payload), mask);    /* 加密 */
        o += strlen(payload);

        uint8_t opcode;
        bool masked;
        size_t plen, hlen;
        int hr = ws_parse_header(frame, o, &opcode, &masked, &plen, &hlen);
        CHECK(hr == 6, "客户端帧头长度 (2+4)");
        CHECK(masked == true, "客户端帧带掩码");
        CHECK(plen == strlen(payload), "客户端帧载荷长度");

        ws_apply_mask(frame + hlen, plen, frame + 2);       /* 解密 */
        CHECK(memcmp(frame + hlen, payload, plen) == 0, "掩码可逆且内容正确");
    }

    {
        /* 只有 1 字节时应该返回"还不够" */
        uint8_t one = 0x81;
        uint8_t opcode;
        bool masked;
        size_t plen, hlen;
        CHECK(ws_parse_header(&one, 1, &opcode, &masked, &plen, &hlen) == 0, "数据不足返回 0");
    }

    if (g_fail == 0) {
        printf("ws_proto: 全部通过\n");
        return 0;
    }
    printf("ws_proto: %d 项失败\n", g_fail);
    return 1;
}
