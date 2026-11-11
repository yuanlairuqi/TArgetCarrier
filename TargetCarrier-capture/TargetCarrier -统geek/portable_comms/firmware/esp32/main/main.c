#include "tc.h"
#include "tc_bus.h"
#include "tc_radio.h"
#include "tc_board.h"
#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include <string.h>
#ifndef TC_ROLE
#define TC_ROLE 1
#endif
void app_main(void) {
    esp_err_t e = nvs_flash_init(); /* preserve NVS on failure; standalone example never erases */
    if (e != ESP_OK)
        return;
    bool radio = tc_radio_init();
    (void)radio;
#if TC_ROLE == 1
    usb_serial_jtag_driver_config_t c = {.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    if (usb_serial_jtag_driver_install(&c) != ESP_OK)
        return;
    static tc_stream parser;
    static tc_queue hi, lo;
    uint8_t tx[128];
    unsigned at = 128;
    uint32_t tx_at = 0, diag_at = 0;
    bool was_connected = false;
#else
    if (!tc_bus_init())
        return;
    static tc_queue hi, lo;
    uint32_t diag_at = 0;
#endif
    for (;;) {
        uint32_t now = (uint32_t)(esp_timer_get_time() / 1000);
        uint8_t b[128];
#if TC_ROLE == 1
        bool connected = usb_serial_jtag_is_connected();
        if (!connected) {
            parser.used = 0;
            hi.used = lo.used = 0;
            at = 128;
        }
        if (connected && !was_connected)
            parser.used = 0;
        was_connected = connected;
        uint8_t in[256];
        int n = usb_serial_jtag_read_bytes(in, sizeof(in), 0);
        for (int i = 0; i < n; i++)
            if (tc_feed(&parser, in[i], b) && connected)
                tc_radio_send(b, now);
        for (int i = 0; i < 8 && tc_radio_receive(b, now); i++)
            if (connected)
                tc_push(b[3] == TC_WAVE ? &lo : &hi, b, now, b[3] == TC_WAVE);
        if (now - diag_at >= 1000 && connected) {
            diag_at = now;
            tc_radio_stats s;
            tc_radio_get_stats(&s);
            uint8_t d[20] = {TC_ROLE, TC_BACKEND, s.configured, s.usable};
            tc_put32(d + 4, s.mac_ok);
            tc_put32(d + 8, s.mac_fail);
            tc_put32(d + 12, s.timeout);
            tc_put32(d + 16, s.rx_drop); /* status session 1 reserved for local diagnostics */
            tc_frame f = {.type = TC_STATUS,
                          .session = 1,
                          .seq = now,
                          .time = now,
                          .total = 20,
                          .len = 20,
                          .count = 1};
            memcpy(f.data, d, 20);
            tc_encode(&f, b);
            tc_push(&hi, b, now, false);
        }
        if (at == 128 && (tc_pop(&hi, tx, now, 100) || tc_pop(&lo, tx, now, 100))) {
            at = 0;
            tx_at = now;
        }
        if (at < 128 && connected) {
            int n = usb_serial_jtag_write_bytes(tx + at, 128 - at, 0);
            if (n > 0)
                at += (unsigned)n;
            if (now - tx_at > 100)
                at = 128;
        }
#else
        /* Two bounded priority queues before entering the transaction backend. */
        for (int i = 0; i < 8 && tc_radio_receive(b, now); i++)
            tc_push(b[3] == TC_WAVE ? &lo : &hi, b, now, b[3] == TC_WAVE);
        if (tc_pop(&hi, b, now, 100) || tc_pop(&lo, b, now, 100))
            tc_bus_send(b, now);
        tc_bus_poll(now);
        for (int i = 0; i < 8 && tc_bus_receive(b, now); i++)
            tc_radio_send(b, now);
        if (now - diag_at >= 1000) {
            diag_at = now;
            tc_radio_stats s;
            tc_radio_get_stats(&s);
            tc_bus_stats bs;
            tc_bus_get_stats(&bs);
            tc_frame f = {.type = TC_STATUS,
                          .session = 1,
                          .seq = now,
                          .time = now,
                          .total = 12,
                          .len = 12,
                          .count = 1};
            f.data[0] = 2;
            f.data[1] = TC_BACKEND;
            f.data[2] = s.configured;
            f.data[3] = s.usable;
            tc_put32(f.data + 4, bs.errors);
            tc_put32(f.data + 8, bs.rx_drop);
            tc_encode(&f, b);
            tc_radio_send(b, now);
        }
#endif
        tc_radio_poll(now);
        vTaskDelay(1);
    }
}
