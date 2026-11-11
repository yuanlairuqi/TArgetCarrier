#ifndef TC_BUS_H
#define TC_BUS_H
#include "tc.h"
bool tc_bus_init(void);
bool tc_bus_send(const uint8_t *slot, uint32_t now);
void tc_bus_poll(uint32_t now);
bool tc_bus_receive(uint8_t *slot, uint32_t now);
typedef struct {
    uint32_t errors, rx_drop, tx_drop, short_transaction;
} tc_bus_stats;
void tc_bus_get_stats(tc_bus_stats *out);
void tc_bus_cancel_pending(void);
#endif
