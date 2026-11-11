#pragma once
#include <stdbool.h>
#include <stdint.h>
/* Diagnostics only. Nothing in this file can start, stop, renew or ack motion.
 *
 * Counters are written from three tasks:
 *   - NimBLE host task : ATT write/read callbacks (status/pulse/diag notify,
 *                        command writes, EXT1/EXT2/EXT3 reads)
 *   - telemetry task   : pulse transfer, telemetry loop gap, msys sampling
 *   - limit_stop task  : uart_link_send() inside the STOP path
 * Only single aligned 8/16/32-bit loads and stores are used, so a read taken
 * while another task increments yields a stale value, never a corrupt one.
 * Counters saturate at their maximum instead of wrapping. No counter
 * allocates memory, takes the robot mutex, or sends anything on the air.
 *
 * Wire contract (frozen with the PC team, all little endian, read-only 20 B):
 *
 * EXT1 6e400006-b5a3-f393-e0a9-e50e24dcca9e  (layout = 1)
 *   [0]=0xE1 [1]=0x01
 *   [2..3]  u16 status_attempt         status notification attempts (100 ms gate)
 *   [4..5]  u16 status_fail            ble_gatts_notify_custom rc != 0
 *   [6..7]  u16 status_enomem          rc == BLE_HS_ENOMEM (6)
 *   [8..9]  u16 status_mbuf_null       ble_hs_mbuf_from_flat() returned NULL
 *   [10..11] u16 pulse_ok
 *   [12..13] u16 pulse_fail            rc != 0
 *   [14..15] u16 pulse_enomem          rc == 6
 *   [16]    u8  pulse_mbuf_null        saturating 255
 *   [17]    u8  last_fail_kind         0 none / 1 status / 2 pulse / 3 diag
 *   [18]    u8  last_fail_rc           NimBLE rc (mbuf NULL recorded as 6)
 *   [19]    u8  last_fail_age_ds       age of last failure, 0.1 s units, 255 = none
 *
 * EXT2 6e400007-b5a3-f393-e0a9-e50e24dcca9e  (layout = 1)
 *   [0]=0xE2 [1]=0x01
 *   [2..5]  u32 ble_cmd_count          ATT writes entering access_command with a
 *                                      valid 16 B payload (bad length not counted)
 *   [6..9]  u32 ble_cmd_last_seq       seq = data[4..7] u32 LE (robot_control.c:45)
 *   [10..11] u16 ble_cmd_last_age_ms   saturating 65535, 0xFFFF = never
 *   [12..13] u16 uart_tx_ok
 *   [14..15] u16 uart_tx_fail
 *   [16..17] u16 att_lock_wait_max_ms  max time access_command waited for the lock
 *   [18]    u8  att_lock_wait_last_ms  saturating 255
 *   [19]    u8  telemetry_gap_max_ms   max telemetry tick interval, saturating 255
 *
 * EXT3 6e400008-b5a3-f393-e0a9-e50e24dcca9e  (layout = 1)
 *   [0]=0xE3 [1]=0x01
 *   [2]     u8  conn_itvl_ms           connection interval, 1.25 ms units -> ms
 *   [3]     u8  conn_latency           connection slave latency
 *   [4..5]  u16 att_mtu                ble_att_mtu(conn), 0 when disconnected
 *   [6..7]  u16 msys_free_min          lowest os_msys_num_free() since boot
 *   [8..9]  u16 msys_free_now          os_msys_num_free() at read time
 *   [10..11] u16 msys_total            os_msys_count()
 *   [12..13] u16 uart_rx_events        uart_link_read() calls that returned bytes
 *   [14..15] u16 diag_ok
 *   [16..17] u16 diag_fail
 *   [18..19] u16 pulse_transfer_abort_count  aborted PULSE transfers
 *
 * Internal, log-only counters (not on the wire): status_ok, msys sampling tick,
 * telemetry_gap_window_max_ms (max gap inside the current 5 s log window).
 */
enum { DIAG_FAIL_NONE = 0, DIAG_FAIL_STATUS = 1, DIAG_FAIL_PULSE = 2, DIAG_FAIL_DIAG = 3 };

typedef struct {
    /* EXT1 */
    uint16_t status_attempt, status_fail, status_enomem, status_mbuf_null;
    uint16_t pulse_ok, pulse_fail, pulse_enomem;
    uint8_t  pulse_mbuf_null;
    uint8_t  last_fail_kind, last_fail_rc;
    uint32_t last_fail_ms;                    /* boot-relative ms, valid if last_fail_kind != 0 */
    uint16_t status_ok;                       /* log only */
    /* EXT2 */
    uint32_t ble_cmd_count, ble_cmd_last_seq, ble_cmd_last_ms;
    uint16_t uart_tx_ok, uart_tx_fail;
    uint16_t att_lock_wait_max_ms;
    uint8_t  att_lock_wait_last_ms;
    uint8_t  telemetry_gap_max_ms;
    uint16_t telemetry_gap_window_max_ms;     /* log only */
    /* EXT3 */
    uint8_t  conn_itvl_ms, conn_latency;
    uint16_t att_mtu;
    uint16_t msys_free_min, msys_free_now, msys_total;
    uint16_t uart_rx_events;
    uint16_t diag_ok, diag_fail;
    uint16_t pulse_transfer_abort_count;
} notify_diag_t;

extern notify_diag_t notify_diag;

uint32_t notify_diag_now_ms(void);
static inline uint16_t notify_diag_inc16(uint16_t value) {
    return value == 0xFFFFU ? value : (uint16_t)(value + 1U);
}
static inline uint8_t notify_diag_inc8(uint8_t value) {
    return value == 0xFFU ? value : (uint8_t)(value + 1U);
}
/* msys_1 + msys_2 free block watermark; 1 Hz from the telemetry task. */
void notify_diag_resource_sample(void);
/* status characteristic notification (telemetry task, 10 Hz gate) */
void notify_diag_status_attempt(void);
void notify_diag_status_ok(void);
void notify_diag_status_fail(int rc);
void notify_diag_status_mbuf_null(void);
/* diagnostic characteristic notification (telemetry task, 1 Hz gate) */
void notify_diag_diag_ok(void);
void notify_diag_diag_fail(int rc);
/* PULSE characteristic notification (telemetry task, <=2 packets per tick) */
void notify_diag_pulse_ok(void);
void notify_diag_pulse_fail(int rc);
void notify_diag_pulse_mbuf_null(void);
void notify_diag_pulse_abort(void);
/* UART bridge (telemetry task and limit_stop task) */
void notify_diag_uart_sent(bool ok);
void notify_diag_uart_received(int bytes);
