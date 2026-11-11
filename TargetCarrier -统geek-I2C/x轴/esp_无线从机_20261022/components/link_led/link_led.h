#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Call once at boot, then publish snapshots from the application task.
 * No LED I/O occurs in publish or in BLE/ESP-NOW callbacks. */
void link_led_init(void);
typedef struct {
    bool ble, geek, have_rx;
    uint32_t rx_packets, last_rx_ms;
    uint32_t tx_ok, tx_fail, errors;
} link_led_snapshot_t;
void link_led_publish(const link_led_snapshot_t *snapshot);
