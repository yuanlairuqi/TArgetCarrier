/**
 * @file    lc_txstats.c
 * @brief   ESP-NOW 提交/回调/应用确认 分离统计
 */

#include "linkcore.h"

#include <string.h>

void lc_txstats_init(lc_txstats_t *s)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
}

bool lc_txstats_submit(lc_txstats_t *s, lc_tx_kind_t kind, uint16_t tag,
                       uint32_t now_ms, bool submit_ok)
{
    if (s == NULL || (unsigned)kind >= (unsigned)LC_TX_KIND_N) {
        return false;
    }

    if (!submit_ok) {
        /* esp_now_send() 没进队列 = 本次提交失败, 不会再有回调 */
        s->submit_fail[kind]++;
        return false;
    }

    s->submit_ok[kind]++;
    /* No packet ID exists in this callback API. Once a callback is missing,
     * resuming FIFO attribution could assign a late A callback to packet B.
     * Keep transport/submission counters running, but mark callbacks unknown
     * until a clean statistics/transport session is established at startup. */
    if (s->cb_desynced) return true;

    if (s->count >= LC_TX_INFLIGHT_MAX) {
        /* 在途表满 (正常不会发生: ESP-NOW 要求等回调再发下一帧) */
        s->dropped_inflight_full++;
        s->cb_desynced = true;
        s->count = 0;
        return false;
    }

    uint8_t idx = (uint8_t)((s->head + s->count) % LC_TX_INFLIGHT_MAX);
    s->kind[idx] = (uint8_t)kind;
    s->tag[idx] = tag;
    s->ms[idx] = now_ms;
    s->count++;
    return true;
}

void lc_txstats_cb(lc_txstats_t *s, bool ok, uint32_t now_ms)
{
    (void)now_ms;
    if (s == NULL) {
        return;
    }

    if (s->cb_desynced || s->count == 0u) {
        /* 没有在途记录: 超时已被注销的迟到回调, 不能算到下一帧头上 */
        s->cb_unexpected++;
        return;
    }

    uint8_t kind = s->kind[s->head];
    s->head = (uint8_t)((s->head + 1u) % LC_TX_INFLIGHT_MAX);
    s->count--;

    if (kind >= (uint8_t)LC_TX_KIND_N) {
        s->cb_unexpected++;
        return;
    }
    if (ok) {
        s->cb_ok[kind]++;
    } else {
        s->cb_fail[kind]++;
    }
}

uint32_t lc_txstats_expire(lc_txstats_t *s, uint32_t now_ms, uint32_t timeout_ms)
{
    if (s == NULL) {
        return 0;
    }

    uint32_t expired = 0;
    uint8_t keep = 0;

    /* 回调顺序与提交顺序一致, 所以超时只可能发生在队首连续若干项 */
    for (uint8_t i = 0; i < s->count; i++) {
        uint8_t idx = (uint8_t)((s->head + i) % LC_TX_INFLIGHT_MAX);
        if (lc_age_exceeds(now_ms, s->ms[idx], timeout_ms)) {
            expired++;
            s->cb_timeout++;
            s->cb_desynced = true;
            /* 超时帧的迟到回调仍可能到来: 记一个"孤儿回调"名额,
             * 避免它被误算成后一帧的结果。 */
            /* No result is attributed after correlation has been lost. */
        } else {
            uint8_t dst = (uint8_t)((s->head + keep) % LC_TX_INFLIGHT_MAX);
            if (dst != idx) {
                s->kind[dst] = s->kind[idx];
                s->tag[dst] = s->tag[idx];
                s->ms[dst] = s->ms[idx];
            }
            keep++;
        }
    }
    s->count = s->cb_desynced ? 0u : keep;
    return expired;
}

void lc_txstats_confirm(lc_txstats_t *s, lc_tx_kind_t kind)
{
    if (s == NULL || (unsigned)kind >= (unsigned)LC_TX_KIND_N) {
        return;
    }
    s->confirm[kind]++;
}
