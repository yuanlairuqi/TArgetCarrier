#include "tc.h"
#include <string.h>
bool tc_wave(tc_emit emit, void *ctx, uint32_t session, uint32_t seq, uint32_t now_ms, uint32_t first,
             uint32_t period, const uint16_t *ids, uint8_t ch, uint8_t samples, uint8_t type,
             const void *values) {
    unsigned size = 12 + 2u * ch + 4u * ch * samples;
    if (!ch || ch > 16 || !samples || size > TC_CHUNK || type < 1 || type > 3 || !period)
        return false;
    uint8_t d[96] = {1, ch, samples, type};
    tc_put32(d + 4, first);
    tc_put32(d + 8, period);
    for (unsigned i = 0; i < ch; i++)
        tc_put16(d + 12 + 2 * i, ids[i]);
    for (unsigned i = 0; i < (unsigned)ch * samples; ++i) {
        uint32_t bits;
        memcpy(&bits, (const uint8_t *)values + 4 * i, 4);
        tc_put32(d + 12 + 2 * ch + 4 * i, bits);
    }
    /* Queue age uses a separate ms clock; deriving it from wrapped us would
       make valid samples expire permanently after the first ~71.6 minutes. */
    return tc_send(emit, ctx, TC_WAVE, session, seq, now_ms, d, (uint16_t)size, true);
}

bool tc_wave_buffer_emit(void *buffer, const uint8_t *slot, bool low, uint32_t now) {
    (void)low;
    tc_wave_buffer *q = buffer;
    return tc_push(&q->frames, slot, now, true);
}
bool tc_wave_flush(tc_wave_buffer *q, tc_emit emit, void *ctx, uint32_t now, uint32_t gap) {
    if (now - q->last_sent < gap || !q->frames.used)
        return false; /* Only retire after downstream has accepted; keep bounded backpressure. */
    tc_item *i = &q->frames.item[q->frames.head];
    if (now - i->at > 100) {
        q->frames.head = (q->frames.head + 1) % TC_Q;
        q->frames.used--;
        q->frames.dropped++;
        return false;
    }
    if (!emit(ctx, i->data, true, now))
        return false;
    q->last_sent = now;
    q->frames.head = (q->frames.head + 1) % TC_Q;
    q->frames.used--;
    return true;
}
