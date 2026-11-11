#include "tc.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
static uint8_t sent[16][128];
static int count;
static bool capture(void *c, const uint8_t *b, bool low, uint32_t now) {
    (void)c;
    (void)low;
    (void)now;
    if (count >= 16)
        return false;
    memcpy(sent[count++], b, 128);
    return true;
}
static void deliver(tc_service *s, uint8_t kind, uint32_t session, uint32_t seq, const uint8_t *d,
                    unsigned n, uint32_t now) {
    count = 0;
    assert(tc_send(capture, 0, kind, session, seq, now, d, n, false));
    uint8_t req[16][128];
    int nr = count;
    memcpy(req, sent, sizeof(req));
    count = 0;
    for (int i = 0; i < nr; i++)
        tc_service_receive(s, req[i], now);
}
int main(void) {
    assert(tc_crc((const uint8_t *)"123456789", 9) == 0xcbf43926u);
    for (unsigned n = 0; n <= 512; n++) {
        uint8_t data[512];
        for (unsigned i = 0; i < n; i++)
            data[i] = (uint8_t)i;
        count = 0;
        assert(tc_send(capture, 0, TC_STATUS, 42, n, 99, data, n, false));
        tc_assembly a = {0};
        for (int i = 0; i < count; i++) {
            tc_frame f;
            assert(tc_decode(sent[i], &f));
            assert(tc_join(&a, &f, 100) == (i + 1 == count ? 1 : 0));
            for (int k = 0; k < 128; k++) {
                uint8_t b[128];
                memcpy(b, sent[i], 128);
                b[k] ^= 1;
                assert(!tc_decode(b, &f));
            }
        }
        assert(a.used == n && !memcmp(a.data, data, n));
    }
    puts("PASS: codec lengths 0..512, CRC bit corruption at every byte");
    count = 0;
    uint8_t data[512] = {0};
    assert(tc_send(capture, 0, TC_STATUS, 42, 8, 9, data, 512, false));
    tc_frame f;
    tc_decode(sent[1], &f);
    tc_assembly a = {0};
    assert(tc_join(&a, &f, 1) == -1);
    tc_decode(sent[0], &f);
    assert(tc_join(&a, &f, 1) == 0);
    tc_decode(sent[2], &f);
    assert(tc_join(&a, &f, 2) == -1);
    tc_decode(sent[0], &f);
    assert(tc_join(&a, &f, 3) == 0);
    tc_decode(sent[1], &f);
    assert(tc_join(&a, &f, 304) == -1);
    tc_stream stream = {0};
    uint8_t out[128];
    for (int i = 0; i < 99; i++)
        assert(!tc_feed(&stream, 0xff, out));
    bool complete = false;
    for (int i = 0; i < 128; i++)
        complete = tc_feed(&stream, sent[0][i], out);
    assert(complete && !memcmp(out, sent[0], 128));
    puts("PASS: missing/out-of-order fragments, reassembly timeout, stream resync");
    tc_queue q = {0};
    for (int i = 0; i < TC_Q; i++)
        assert(tc_push(&q, sent[0], 0xfffffff0u, false));
    assert(!tc_push(&q, sent[0], 1, false));
    assert(tc_push(&q, sent[0], 2, true));
    assert(q.used == TC_Q && q.dropped == 2);
    assert(tc_pop(&q, out, 3, 100));
    assert(!tc_pop(&q, out, 200, 100));
    puts("PASS: bounded queue overflow, replacement, expiry, millis wrap");
    tc_param p[] = {{1, TC_U32, 1, 25, 1, 1000},
                    {2, TC_F32, 1, 0x3f800000, 0, 100},
                    {3, TC_I32, 0, 4, -10, 10}};
    tc_service s;
    tc_service_init(&s, p, 3, capture, 0);
    deliver(&s, TC_HELLO, 77, 1, 0, 0, 1);
    assert(count == 1 && s.established);
    deliver(&s, TC_HELLO, 88, 1, 0, 0, 2);
    assert(count == 0 && s.session == 77);
    uint8_t write[] = {1, 0, 1, 0, 42, 0, 0, 0};
    deliver(&s, TC_WRITE, 77, 2, write, 8, 3);
    assert(p[0].bits == 42 && count == 1 && sent[0][29] == TC_OK && sent[0][35] == 1);
    p[0].bits = 43;
    deliver(&s, TC_WRITE, 77, 2, write, 8, 4);
    assert(p[0].bits == 43 && s.duplicate == 1 && sent[0][36] == 42);
    write[4] = 44;
    deliver(&s, TC_WRITE, 77, 2, write, 8, 5);
    assert(count == 0 && p[0].bits == 43);
    deliver(&s, TC_WRITE, 77, 1, write, 8, 6);
    assert(count == 0 && p[0].bits == 43);
    tc_put32(write + 4, 1001);
    deliver(&s, TC_WRITE, 77, 3, write, 8, 7);
    assert(sent[0][29] == TC_RANGE && p[0].bits == 43);
    write[0] = 2;
    write[2] = 3;
    tc_put32(write + 4, 0x7fc00000);
    deliver(&s, TC_WRITE, 77, 4, write, 8, 8);
    assert(sent[0][29] == TC_RANGE && p[1].bits == 0x3f800000);
    write[0] = 3;
    write[2] = 2;
    tc_put32(write + 4, 5);
    deliver(&s, TC_WRITE, 77, 5, write, 8, 9);
    assert(sent[0][29] == TC_BAD && p[2].bits == 4);
    uint8_t id[] = {99, 0};
    deliver(&s, TC_READ, 77, 6, id, 2, 10);
    assert(sent[0][29] == TC_UNKNOWN);
    deliver(&s, TC_CONTROL, 77, 7, id, 2, 11);
    assert(sent[0][29] == TC_UNSUPPORTED);
    tc_service_tick(&s, 2012);
    assert(!s.established);
    deliver(&s, TC_WRITE, 77, 8, write, 8, 2013);
    assert(!count);
    deliver(&s, TC_HELLO, 88, 9, 0, 0, 2014);
    assert(s.established && s.session == 88);
    puts("PASS: parameter apply/range/NaN/type/read-only/unknown, cached duplicate, stale "
         "sequence, session expiry");
    uint16_t ids[] = {1, 2};
    uint32_t values[] = {3, 4};
    count = 0;
    assert(tc_wave(capture, 0, 88, 1, 1, 1234, 40000, ids, 2, 1, TC_U32, values));
    assert(sent[0][3] == TC_WAVE);
    assert(!tc_wave(capture, 0, 88, 1, 1, 0, 1, ids, 2, 12, 1, values));
    puts("PASS: waveform metadata and bounded payload");
    tc_wave_buffer waves = {0};
    for (int i = 0; i < TC_Q + 5; i++)
        assert(tc_wave_buffer_emit(&waves, sent[0], true, 1));
    assert(waves.frames.used == TC_Q && waves.frames.dropped == 5);
    assert(!tc_wave_flush(&waves, capture, 0, 5, 20));
    assert(tc_wave_flush(&waves, capture, 0, 21, 20));
    assert(!tc_wave_flush(&waves, capture, 0, 200, 20));
    puts("PASS: acquisition/send decoupling, rate limit, bounded freshness");
    FILE *file = fopen("golden.bin", "wb");
    assert(file);
    fwrite(sent[0], 1, 128, file);
    fclose(file);
    tc_wave_buffer wrapped = {0};
    uint32_t uptime = 4300000u;
    assert(tc_wave(tc_wave_buffer_emit, &wrapped, 88, 2, uptime, uptime * 1000u,
                   40000, ids, 2, 1, TC_U32, values));
    assert(wrapped.frames.item[0].at == uptime);
    assert(tc_wave_flush(&wrapped, capture, NULL, uptime + 1, 20));
    puts("PASS: microsecond timestamp wrap does not expire millisecond queue age");
    return 0;
}
