#ifndef TC_RADIO_H
#define TC_RADIO_H
#include "tc.h"
bool tc_radio_init(void);
bool tc_radio_send(const uint8_t *slot, uint32_t now);
void tc_radio_poll(uint32_t now);
bool tc_radio_receive(uint8_t *slot, uint32_t now);
typedef struct {
    uint32_t mac_ok, mac_fail, rx_drop, auth_fail, timeout, expired;
    bool configured, usable;
} tc_radio_stats;
void tc_radio_get_stats(tc_radio_stats *out);
#endif
