/**
 * @file    ws_proto.h
 * @brief   极简 WebSocket 协议工具 (RFC 6455) —— 纯 C, 不依赖 Arduino
 *
 *  只实现装机需要的子集:
 *    - 服务端握手: Sec-WebSocket-Accept = base64(sha1(key + GUID))
 *    - 服务端发文本帧 (不掩码)
 *    - 收客户端帧 (客户端必须掩码)
 *  不支持分片、扩展、压缩; 控制帧只处理 ping/pong/close。
 *
 *  ★ 单独抽出来的原因: 这部分可以用**主机端单元测试**验证 (sha1/base64/
 *    帧编解码都有标准向量), 不必等到板子上再猜。
 */

#ifndef WS_PROTO_H
#define WS_PROTO_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WS_OP_CONT   0x0u
#define WS_OP_TEXT   0x1u
#define WS_OP_BIN    0x2u
#define WS_OP_CLOSE  0x8u
#define WS_OP_PING   0x9u
#define WS_OP_PONG   0xAu

/** SHA-1 摘要 (out 20 字节) */
void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20]);

/** 标准 Base64 编码 (不带换行)。out 至少 ((len+2)/3)*4 + 1 字节; 返回写入长度 */
size_t ws_base64(const uint8_t *in, size_t len, char *out, size_t cap);

/** 由客户端的 Sec-WebSocket-Key 算出握手应答 (29 字符 + '\0')。成功返回 true */
bool ws_accept_key(const char *client_key, char out[29]);

/** 编码一帧"服务端→客户端"文本帧 (FIN=1, 不掩码)。返回总字节数, 0=cap 不够 */
size_t ws_encode_text(const char *payload, size_t len, uint8_t *out, size_t cap);

/**
 * 解析一帧的头部。
 * @return >0 头部字节数; 0 = 数据还不够; -1 = 非法帧
 */
int ws_parse_header(const uint8_t *buf, size_t len, uint8_t *opcode,
                    bool *masked, size_t *payload_len, size_t *header_len);

/** 用 4 字节掩码就地解开载荷 (加解密同一操作) */
void ws_apply_mask(uint8_t *payload, size_t len, const uint8_t key[4]);

#ifdef __cplusplus
}
#endif

#endif /* WS_PROTO_H */
