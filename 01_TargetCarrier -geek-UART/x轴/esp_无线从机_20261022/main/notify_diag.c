#include "notify_diag.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "os/os_mbuf.h"

notify_diag_t notify_diag;

uint32_t notify_diag_now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000);
}

static void note_fail(uint8_t kind, int rc, uint32_t now)
{
    notify_diag.last_fail_kind = kind;
    notify_diag.last_fail_rc = (uint8_t)rc;
    notify_diag.last_fail_ms = now;
}

void notify_diag_status_attempt(void)
{
    notify_diag.status_attempt = notify_diag_inc16(notify_diag.status_attempt);
}

void notify_diag_status_ok(void)
{
    notify_diag.status_ok = notify_diag_inc16(notify_diag.status_ok);
}

void notify_diag_status_fail(int rc)
{
    notify_diag.status_fail = notify_diag_inc16(notify_diag.status_fail);
    if (rc == BLE_HS_ENOMEM) {
        notify_diag.status_enomem = notify_diag_inc16(notify_diag.status_enomem);
    }
    note_fail(DIAG_FAIL_STATUS, rc, notify_diag_now_ms());
}

void notify_diag_status_mbuf_null(void)
{
    notify_diag.status_mbuf_null = notify_diag_inc16(notify_diag.status_mbuf_null);
    /* A null payload mbuf is also a pool-exhaustion symptom, not a notify rc. */
    note_fail(DIAG_FAIL_STATUS, BLE_HS_ENOMEM, notify_diag_now_ms());
}

void notify_diag_diag_ok(void)
{
    notify_diag.diag_ok = notify_diag_inc16(notify_diag.diag_ok);
}

void notify_diag_diag_fail(int rc)
{
    notify_diag.diag_fail = notify_diag_inc16(notify_diag.diag_fail);
    note_fail(DIAG_FAIL_DIAG, rc, notify_diag_now_ms());
}

void notify_diag_pulse_ok(void)
{
    notify_diag.pulse_ok = notify_diag_inc16(notify_diag.pulse_ok);
}

void notify_diag_pulse_fail(int rc)
{
    notify_diag.pulse_fail = notify_diag_inc16(notify_diag.pulse_fail);
    if (rc == BLE_HS_ENOMEM) {
        notify_diag.pulse_enomem = notify_diag_inc16(notify_diag.pulse_enomem);
    }
    note_fail(DIAG_FAIL_PULSE, rc, notify_diag_now_ms());
}

void notify_diag_pulse_mbuf_null(void)
{
    notify_diag.pulse_mbuf_null = notify_diag_inc8(notify_diag.pulse_mbuf_null);
    note_fail(DIAG_FAIL_PULSE, BLE_HS_ENOMEM, notify_diag_now_ms());
}

void notify_diag_pulse_abort(void)
{
    notify_diag.pulse_transfer_abort_count =
        notify_diag_inc16(notify_diag.pulse_transfer_abort_count);
}

void notify_diag_uart_sent(bool ok)
{
    if (ok) {
        notify_diag.uart_tx_ok = notify_diag_inc16(notify_diag.uart_tx_ok);
    } else {
        notify_diag.uart_tx_fail = notify_diag_inc16(notify_diag.uart_tx_fail);
    }
}

void notify_diag_uart_received(int bytes)
{
    if (bytes > 0) {
        notify_diag.uart_rx_events = notify_diag_inc16(notify_diag.uart_rx_events);
    }
}

void notify_diag_resource_sample(void)
{
    /* os_msys_count()/os_msys_num_free() walk the registered msys pools and
     * read their mempool counters; they allocate nothing and take no lock.
     * See IDF components/bt/host/nimble/nimble/porting/nimble/src/os_mbuf.c. */
    int total = os_msys_count();
    int free_blocks = os_msys_num_free();
    if (total > 0 && total < 0x10000) {
        notify_diag.msys_total = (uint16_t)total;
    }
    if (free_blocks >= 0 && free_blocks < 0x10000) {
        uint16_t f = (uint16_t)free_blocks;
        notify_diag.msys_free_now = f;
        if (notify_diag.msys_free_min == 0 || f < notify_diag.msys_free_min) {
            notify_diag.msys_free_min = f;
        }
    }
}
