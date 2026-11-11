/**
 * @file    ws_proto.c
 * @brief   极简 WebSocket 协议工具实现 (SHA-1 / Base64 / 帧)
 *
 *  这些函数都是纯计算, 没有全局状态, 所以能在主机上跑单元测试:
 *      靶车/firmware/esp32_master/test/ws_proto_test.c
 */

#include "ws_proto.h"

#include <string.h>

/* ================================================================== */
/* SHA-1                                                              */
/* ================================================================== */

#define ROL(v, n) (((v) << (n)) | ((v) >> (32 - (n))))

static void sha1_block(uint32_t h[5], const uint8_t blk[64])
{
    uint32_t w[80];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)blk[i * 4] << 24) | ((uint32_t)blk[i * 4 + 1] << 16) |
               ((uint32_t)blk[i * 4 + 2] << 8) | (uint32_t)blk[i * 4 + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = ROL(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        uint32_t tmp = ROL(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = ROL(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

void ws_sha1(const uint8_t *data, size_t len, uint8_t out[20])
{
    uint32_t h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    size_t i = 0;
    for (; i + 64 <= len; i += 64) {
        sha1_block(h, data + i);
    }

    /* 尾部: 剩余字节 + 0x80 + 补零 + 64 位长度 (大端) */
    uint8_t tail[128];
    size_t rem = len - i;
    memcpy(tail, data + i, rem);
    tail[rem] = 0x80u;
    size_t tail_len = (rem + 1 + 8 <= 64) ? 64 : 128;
    memset(tail + rem + 1, 0, tail_len - rem - 1 - 8);

    uint64_t bits = (uint64_t)len * 8u;
    for (int k = 0; k < 8; k++) {
        tail[tail_len - 1 - k] = (uint8_t)(bits >> (8 * k));
    }
    for (size_t off = 0; off < tail_len; off += 64) {
        sha1_block(h, tail + off);
    }

    for (int k = 0; k < 5; k++) {
        out[k * 4]     = (uint8_t)(h[k] >> 24);
        out[k * 4 + 1] = (uint8_t)(h[k] >> 16);
        out[k * 4 + 2] = (uint8_t)(h[k] >> 8);
        out[k * 4 + 3] = (uint8_t)(h[k]);
    }
}

/* ================================================================== */
/* Base64                                                             */
/* ================================================================== */

static const char B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

size_t ws_base64(const uint8_t *in, size_t len, char *out, size_t cap)
{
    size_t need = ((len + 2) / 3) * 4;
    if (cap < need + 1) {
        return 0;
    }

    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        uint32_t v = ((uint32_t)in[i] << 16) | ((uint32_t)in[i + 1] << 8) | in[i + 2];
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = B64[(v >> 6) & 0x3F];
        out[o++] = B64[v & 0x3F];
    }
    if (i < len) {
        uint32_t v = ((uint32_t)in[i] << 16);
        if (i + 1 < len) {
            v |= ((uint32_t)in[i + 1] << 8);
        }
        out[o++] = B64[(v >> 18) & 0x3F];
        out[o++] = B64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < len) ? B64[(v >> 6) & 0x3F] : '=';
        out[o++] = '=';
    }
    out[o] = '\0';
    return o;
}

/* ================================================================== */
/* 握手                                                               */
/* ================================================================== */

bool ws_accept_key(const char *client_key, char out[29])
{
    static const char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    if (client_key == NULL || out == NULL) {
        return false;
    }

    char joined[64];
    size_t kl = strlen(client_key);
    size_t gl = sizeof(GUID) - 1;
    if (kl + gl + 1 > sizeof(joined)) {
        return false;
    }
    memcpy(joined, client_key, kl);
    memcpy(joined + kl, GUID, gl);
    joined[kl + gl] = '\0';

    uint8_t digest[20];
    ws_sha1((const uint8_t *)joined, kl + gl, digest);

    char b64[32];
    size_t n = ws_base64(digest, sizeof(digest), b64, sizeof(b64));
    if (n != 28) {
        return false;
    }
    memcpy(out, b64, n);
    out[n] = '\0';
    return true;
}

/* ================================================================== */
/* 帧                                                                 */
/* ================================================================== */

size_t ws_encode_text(const char *payload, size_t len, uint8_t *out, size_t cap)
{
    size_t need = 2 + (len < 126 ? 0 : (len <= 0xFFFF ? 2 : 8)) + len;
    if (cap < need) {
        return 0;
    }

    size_t o = 0;
    out[o++] = 0x80u | WS_OP_TEXT;         /* FIN=1, opcode=1 */
    if (len < 126) {
        out[o++] = (uint8_t)len;
    } else if (len <= 0xFFFF) {
        out[o++] = 126u;
        out[o++] = (uint8_t)(len >> 8);
        out[o++] = (uint8_t)(len);
    } else {
        out[o++] = 127u;
        for (int k = 7; k >= 0; k--) {
            out[o++] = (uint8_t)((uint64_t)len >> (8 * k));
        }
    }
    memcpy(out + o, payload, len);
    return o + len;
}

int ws_parse_header(const uint8_t *buf, size_t len, uint8_t *opcode,
                    bool *masked, size_t *payload_len, size_t *header_len)
{
    if (buf == NULL || len < 2) {
        return 0;
    }

    uint8_t b0 = buf[0];
    uint8_t b1 = buf[1];
    bool m = (b1 & 0x80u) != 0;
    uint8_t plen7 = (uint8_t)(b1 & 0x7Fu);
    size_t hl = 2;
    uint64_t plen = plen7;

    if (plen7 == 126) {
        if (len < 4) return 0;
        plen = ((uint64_t)buf[2] << 8) | buf[3];
        hl = 4;
    } else if (plen7 == 127) {
        if (len < 10) return 0;
        plen = 0;
        for (int k = 0; k < 8; k++) {
            plen = (plen << 8) | buf[2 + k];
        }
        hl = 10;
    }
    if (m) {
        hl += 4;
    }
    if (len < hl) {
        return 0;
    }
    if (plen > 1024u * 1024u) {   /* 我们不需要大帧, 防御一下 */
        return -1;
    }

    if (opcode)      *opcode = (uint8_t)(b0 & 0x0Fu);
    if (masked)      *masked = m;
    if (payload_len) *payload_len = (size_t)plen;
    if (header_len)  *header_len = hl;
    return (int)hl;
}

void ws_apply_mask(uint8_t *payload, size_t len, const uint8_t key[4])
{
    for (size_t i = 0; i < len; i++) {
        payload[i] ^= key[i & 3u];
    }
}
