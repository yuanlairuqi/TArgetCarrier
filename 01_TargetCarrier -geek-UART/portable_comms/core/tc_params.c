#include "tc.h"
#include <string.h>
#include <math.h>
void tc_service_init(tc_service *s, tc_param *p, size_t n, tc_emit emit, void *ctx) {
    memset(s, 0, sizeof(*s));
    s->params = p;
    s->param_count = n;
    s->emit = emit;
    s->ctx = ctx;
}
void tc_service_tick(tc_service *s, uint32_t now) {
    if (s->rx.next && now - s->rx.at > 300) {
        s->rx.next = 0;
        s->reassembly_timeout++;
    }
    if (s->established && now - s->last_seen > 2000) {
        s->established = false;
        s->rx.next = 0;
        s->cached_len = 0;
    }
}
void tc_service_receive(tc_service *s, const uint8_t *b, uint32_t now) {
    tc_frame f;
    if (!tc_decode(b, &f)) {
        s->errors++;
        return;
    }
    tc_service_tick(s, now);
    int joined = tc_join(&s->rx, &f, now);
    if (joined < 0) {
        s->errors++;
        return;
    }
    if (!joined)
        return;
    uint8_t *d = s->rx.data;
    uint16_t n = s->rx.used;
    if (f.type ==
        TC_HELLO) { /* Only empty HELLO opens a session; same-session replay cannot rewind. */
        if (n)
            return;
        if (s->established && f.session != s->session)
            return;
        if (!s->established) {
            s->session = f.session;
            s->last_seq = f.seq;
            s->cached_len = 0;
            s->established = true;
        } else if ((int32_t)(f.seq - s->last_seq) > 0)
            s->last_seq = f.seq;
        s->last_seen = now;
        uint8_t hello[8] = {TC_HELLO, TC_OK, 0, 0, 2, 0, 0, 0};
        tc_send(s->emit, s->ctx, TC_RESULT, f.session, f.seq, now, hello, 8, false);
        return;
    }
    if (!s->established || f.session != s->session)
        return;
    if (f.seq == s->last_seq) {
        if (s->cached_len && f.type == s->cached_type && n == s->cached_request_len &&
            !memcmp(d, s->cached_request, n)) {
            s->duplicate++;
            tc_send(s->emit, s->ctx, TC_RESULT, f.session, f.seq, now, s->cached, s->cached_len,
                    false);
        }
        return;
    }
    if ((int32_t)(f.seq - s->last_seq) <= 0)
        return;
    if (f.type != TC_READ && f.type != TC_WRITE && f.type != TC_CONTROL)
        return;
    s->last_seen = now;
    s->last_seq = f.seq;
    s->cached_type = f.type;
    s->cached_request_len = n;
    memcpy(s->cached_request, d, n);
    uint8_t *r = s->cached;
    memset(r, 0, 32);
    r[0] = f.type;
    r[1] = TC_BAD;
    r[7] = 0; /* stage=0 rejected, 1 applied; never claims business execution */
    if (n >= 2) {
        uint16_t id = tc_get16(d);
        tc_put16(r + 2, id);
        tc_param *p = NULL;
        for (size_t i = 0; i < s->param_count; i++)
            if (s->params[i].id == id)
                p = &s->params[i];
        r[1] = TC_UNKNOWN;
        if (f.type == TC_CONTROL) {
            r[1] = TC_UNSUPPORTED;
            if (s->control) {
                tc_command_result result = s->control(s->ctx, id, d + 2, n - 2);
                r[1] = result.code;
                r[7] = result.stage;
                tc_put32(r + 8, result.detail);
            }
        } else if (p) {
            r[4] = p->type;
            r[1] = TC_BAD;
            if (f.type == TC_READ && n == 2) {
                r[1] = TC_OK;
                r[7] = 1;
            }
            if (f.type == TC_WRITE && n == 8 && d[2] == p->type && d[3] == 0 && p->writable) {
                uint32_t bits = tc_get32(d + 4);
                double v;
                if (p->type == TC_F32) {
                    float x;
                    memcpy(&x, &bits, 4);
                    v = x;
                } else if (p->type == TC_I32)
                    v = (int32_t)bits;
                else
                    v = bits;
                r[1] = TC_RANGE;
                if (isfinite(v) && v >= p->minimum && v <= p->maximum) {
                    p->bits = bits;
                    r[1] = TC_OK;
                    r[7] = 1;
                }
            }
            tc_put32(r + 8, p->bits);
        }
    }
    s->cached_len = 12;
    tc_send(s->emit, s->ctx, TC_RESULT, f.session, f.seq, now, r, 12, false);
}
