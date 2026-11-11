#include "tc.h"
#include <string.h>
#include <math.h>
uint16_t tc_get16(const uint8_t *p) {
    return (uint16_t)(p[0] | (uint16_t)p[1] << 8);
}
uint32_t tc_get32(const uint8_t *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
void tc_put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void tc_put32(uint8_t *p, uint32_t v) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}
uint32_t tc_crc(const uint8_t *p, size_t n) {
    uint32_t c = ~0u;
    while (n--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320u & (0u - (c & 1)));
    }
    return ~c;
}
static bool valid(const tc_frame *f) {
    return f->type >= 1 && f->type <= 7 && f->session && f->total <= TC_MAX &&
           f->count == (f->total ? ((f->total + 95) / 96) : 1) && f->index < f->count &&
           f->len == (f->index + 1 < f->count ? 96 : f->total - f->index * 96);
}
bool tc_encode(const tc_frame *f, uint8_t *b) {
    if (!valid(f))
        return false;
    memset(b, 0, 128);
    b[0] = 'T';
    b[1] = 'C';
    b[2] = 2;
    b[3] = f->type;
    tc_put32(b + 4, f->session);
    tc_put32(b + 8, f->seq);
    tc_put32(b + 12, f->time);
    tc_put16(b + 16, f->total);
    b[18] = f->index;
    b[19] = f->count;
    tc_put16(b + 20, f->len);
    memcpy(b + 28, f->data, f->len);
    tc_put32(b + 124, tc_crc(b, 124));
    return true;
}
bool tc_decode(const uint8_t *b, tc_frame *f) {
    if (b[0] != 'T' || b[1] != 'C' || b[2] != 2 || tc_get32(b + 124) != tc_crc(b, 124))
        return false;
    for (int i = 22; i < 28; i++)
        if (b[i])
            return false;
    *f = (tc_frame){.type = b[3],
                    .session = tc_get32(b + 4),
                    .seq = tc_get32(b + 8),
                    .time = tc_get32(b + 12),
                    .total = tc_get16(b + 16),
                    .index = b[18],
                    .count = b[19],
                    .len = tc_get16(b + 20)};
    if (!valid(f))
        return false;
    for (unsigned i = 28 + f->len; i < 124; i++)
        if (b[i])
            return false;
    memcpy(f->data, b + 28, f->len);
    return true;
}
bool tc_feed(tc_stream *s, uint8_t b, uint8_t *out) {
    s->bytes[s->used++] = b;
    if (s->used < 128)
        return false;
    tc_frame f;
    if (tc_decode(s->bytes, &f)) {
        memcpy(out, s->bytes, 128);
        s->used = 0;
        return true;
    }
    memmove(s->bytes, s->bytes + 1, 127);
    s->used = 127;
    s->bad++;
    return false;
}
bool tc_push(tc_queue *q, const uint8_t *b, uint32_t now, bool oldest) {
    if (q->used == TC_Q) {
        q->dropped++;
        if (!oldest)
            return false;
        q->head = (q->head + 1) % TC_Q;
        q->used--;
    }
    tc_item *i = &q->item[(q->head + q->used) % TC_Q];
    memcpy(i->data, b, 128);
    i->at = now;
    q->used++;
    if (q->used > q->peak)
        q->peak = q->used;
    return true;
}
bool tc_pop(tc_queue *q, uint8_t *b, uint32_t now, uint32_t age) {
    while (q->used) {
        tc_item *i = &q->item[q->head];
        q->head = (q->head + 1) % TC_Q;
        q->used--;
        if (now - i->at > age) {
            q->dropped++;
            continue;
        }
        memcpy(b, i->data, 128);
        return true;
    }
    return false;
}
int tc_join(tc_assembly *a, const tc_frame *f, uint32_t now) {
    if (!valid(f))
        return -1;
    if (a->next && now - a->at > 300) {
        a->next = 0;
        if (f->index)
            return -1;
    }
    if (f->index == 0) {
        a->session = f->session;
        a->seq = f->seq;
        a->type = f->type;
        a->total = f->total;
        a->count = f->count;
        a->used = 0;
        a->next = 0;
        a->at = now;
    }
    if (f->session != a->session || f->seq != a->seq || f->type != a->type ||
        f->total != a->total || f->count != a->count || f->index != a->next) {
        a->next = 0;
        return -1;
    }
    memcpy(a->data + a->used, f->data, f->len);
    a->used += f->len;
    a->next++;
    if (a->next == a->count) {
        a->next = 0;
        return 1;
    }
    return 0;
}
bool tc_send(tc_emit emit, void *ctx, uint8_t type, uint32_t session, uint32_t seq, uint32_t time,
             const uint8_t *d, uint16_t n, bool low) {
    if (n > TC_MAX || (!d && n) || !emit)
        return false;
    tc_frame f = {.type = type,
                  .session = session,
                  .seq = seq,
                  .time = time,
                  .total = n,
                  .count = n ? (uint8_t)((n + 95) / 96) : 1};
    uint8_t b[128];
    for (f.index = 0; f.index < f.count; f.index++) {
        unsigned off = f.index * 96;
        f.len = n - off > 96 ? 96 : n - off;
        if (f.len)
            memcpy(f.data, d + off, f.len);
        if (!tc_encode(&f, b) || !emit(ctx, b, low, time))
            return false;
    }
    return true;
}
