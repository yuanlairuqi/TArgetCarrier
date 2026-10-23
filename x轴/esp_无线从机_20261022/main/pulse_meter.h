#ifndef PULSE_METER_H
#define PULSE_METER_H
#include <stdbool.h>
#include <stdint.h>
/* Caller serializes ISR edge updates and task reads. 500 ms is a freshness
 * bound, not proof that a driver or motor has stopped. */
typedef struct {
    uint32_t tick, period;
    int64_t edge_us;
    bool seen, pair;
} pulse_meter_t;
static inline void pulse_meter_edge(pulse_meter_t *m,uint32_t tick,int64_t now)
{
    m->pair=m->seen && now>=m->edge_us && now-m->edge_us<=500000;
    m->period=tick-m->tick;
    m->tick=tick;m->edge_us=now;m->seen=true;
}
static inline bool pulse_meter_read(const pulse_meter_t *m,uint32_t clock_hz,
                                   int64_t now,uint32_t *hz)
{
    bool recent=m->pair && m->period && clock_hz && now>=m->edge_us && now-m->edge_us<=500000;
    *hz=recent?(uint32_t)(((uint64_t)clock_hz+m->period/2U)/m->period):0;
    return recent;
}
#endif
