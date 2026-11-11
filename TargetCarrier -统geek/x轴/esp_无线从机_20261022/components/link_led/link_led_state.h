#pragma once
#include "link_led.h"
enum { LINK_LED_DARK, LINK_LED_RED, LINK_LED_GREEN, LINK_LED_BLUE };
static inline unsigned link_led_connection(const link_led_snapshot_t *s, uint32_t age, uint32_t now)
{
    if (age <= 1000U && (s->ble || s->geek)) return LINK_LED_GREEN;
    return ((now / 500U) & 1U) ? LINK_LED_DARK : LINK_LED_RED;
}
/* Application freshness and local MAC outcomes, not RSSI or motor safety. */
static inline unsigned link_led_quality(const link_led_snapshot_t *s, uint32_t now,
                                        uint32_t ok, uint32_t fail, uint32_t errors)
{
    if (!s->have_rx) return 0;
    uint32_t age = (uint32_t)(now - s->last_rx_ms);
    if (age > 600U || (s->geek && fail && (uint64_t)fail * 5U >= (uint64_t)ok + fail)) return 3;
    if (age > 200U || (s->geek && (fail || errors))) return 2;
    return 1;
}
static inline bool link_led_activity(uint32_t rate, uint32_t now)
{
    if (!rate) return false;
    uint32_t period = rate < 5U ? 1000U : rate < 15U ? 400U : 100U;
    return now % period < 50U;
}
static inline bool link_led_quality_on(unsigned quality, uint32_t now)
{
    return quality == 1 || (quality == 2 && now % 1000U < 500U) ||
           (quality == 3 && now % 250U < 125U);
}
