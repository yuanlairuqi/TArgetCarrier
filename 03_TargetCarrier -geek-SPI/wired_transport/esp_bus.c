#include "tc_bus.h"
#include "tc_board.h"
#include <string.h>
#include "driver/uart.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c_master.h"
#include "esp_attr.h"
static tc_queue out, outlo, hi, lo;
static tc_stream stream;
static tc_bus_stats stats;
static uint32_t last_poll;
static spi_device_handle_t spi;
static i2c_master_bus_handle_t i2c;
static i2c_master_dev_handle_t device;
static DMA_ATTR uint8_t tx[128], rx[128];
#if TC_BACKEND == TC_SPI
/* One bounded in-flight frame is retransmitted until STM confirms queue receipt. */
static bool spi_inflight;
static uint32_t spi_started;
#endif
bool tc_bus_init(void) {
#if TC_BACKEND == TC_UART
    uart_config_t c = {.baud_rate = TC_UART_BAUD,
                       .data_bits = UART_DATA_8_BITS,
                       .parity = UART_PARITY_DISABLE,
                       .stop_bits = UART_STOP_BITS_1,
                       .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
                       .source_clk = UART_SCLK_DEFAULT};
    return uart_driver_install(UART_NUM_1, 1024, 1024, 0, NULL, 0) == ESP_OK &&
           uart_param_config(UART_NUM_1, &c) == ESP_OK &&
           uart_set_pin(UART_NUM_1, TC_ESP_TX, TC_ESP_RX, -1, -1) == ESP_OK;
#else
#if TC_BACKEND == TC_SPI
    spi_bus_config_t c = {.mosi_io_num = TC_ESP_MOSI,
                          .miso_io_num = TC_ESP_MISO,
                          .sclk_io_num = TC_ESP_SCK,
                          .quadwp_io_num = -1,
                          .quadhd_io_num = -1,
                          .max_transfer_sz = 128};
    spi_device_interface_config_t d = {.clock_speed_hz = TC_SPI_HZ,
                                       .mode = 0,
                                       .spics_io_num = TC_ESP_CS,
                                       .queue_size = 1,
                                       .cs_ena_posttrans = 4};
    return spi_bus_initialize(SPI2_HOST, &c, SPI_DMA_CH_AUTO) == ESP_OK &&
           spi_bus_add_device(SPI2_HOST, &d, &spi) == ESP_OK;
#else
    gpio_config_t g = {.pin_bit_mask = 1ULL << TC_ESP_READY,
                       .mode = GPIO_MODE_INPUT,
                       .pull_down_en = GPIO_PULLDOWN_ENABLE};
    if (gpio_config(&g) != ESP_OK)
        return false;
    i2c_master_bus_config_t c = {.i2c_port = I2C_NUM_1,
                                 .sda_io_num = TC_ESP_SDA,
                                 .scl_io_num = TC_ESP_SCL,
                                 .clk_source = I2C_CLK_SRC_DEFAULT,
                                 .glitch_ignore_cnt = 7};
    i2c_device_config_t d = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
                             .device_address = TC_I2C_ADDR,
                             .scl_speed_hz = TC_I2C_HZ};
    return i2c_new_master_bus(&c, &i2c) == ESP_OK &&
           i2c_master_bus_add_device(i2c, &d, &device) == ESP_OK;
#endif
#endif
}
bool tc_bus_send(const uint8_t *b, uint32_t now) {
    bool ok = tc_push(b[3] == TC_WAVE ? &outlo : &out, b, now, b[3] == TC_WAVE);
    if (!ok)
        stats.tx_drop++;
    return ok;
}
static void accept(const uint8_t *b, uint32_t now) {
    tc_frame f;
    if (tc_decode(b, &f)) {
        if (!tc_push(f.type == TC_WAVE ? &lo : &hi, b, now, f.type == TC_WAVE))
            stats.rx_drop++;
    }
}
void tc_bus_poll(uint32_t now) {
#if TC_BACKEND == TC_UART
    uint8_t bytes[256], b[128];
    int n = uart_read_bytes(UART_NUM_1, bytes, sizeof(bytes), 0);
    for (int i = 0; i < n; i++)
        if (tc_feed(&stream, bytes[i], b))
            accept(b, now);
    if (uart_wait_tx_done(UART_NUM_1, 0) == ESP_OK &&
        (tc_pop(&out, tx, now, 100) || tc_pop(&outlo, tx, now, 100))) {
        if (uart_write_bytes(UART_NUM_1, tx, 128) != 128)
            stats.errors++;
    }
#else
    if (now - last_poll < TC_POLL_MS)
        return;
    last_poll = now;
#if TC_BACKEND == TC_SPI
    if (spi_inflight && now - spi_started > TC_QUEUE_AGE_MS) {
        spi_inflight = false;
        stats.tx_drop++;
    }
    if (!spi_inflight && (tc_pop(&out, tx, now, TC_QUEUE_AGE_MS) ||
                          tc_pop(&outlo, tx, now, TC_QUEUE_AGE_MS))) {
        spi_inflight = true;
        spi_started = now;
    }
    if (!spi_inflight)
        memset(tx, 0, 128); /* Poll the STM mailbox even with no pending command. */
    spi_transaction_t t = {.length = 1024, .rxlength = 1024, .tx_buffer = tx, .rx_buffer = rx};
    if (spi_device_transmit(spi, &t) == ESP_OK) {
        tc_frame f;
        if (tc_decode(rx, &f)) {
            /* SA acknowledges only transport queue receipt; never application execution. */
            if (f.type == TC_HELLO && f.len == 6 && f.data[0] == 'S' && f.data[1] == 'A') {
                if (spi_inflight && f.session == tc_get32(tx + 4) &&
                    f.seq == tc_get32(tx + 8) && tc_get32(f.data + 2) == tc_get32(tx + 124))
                    spi_inflight = false;
            } else
                accept(rx, now);
        }
    } else
        stats.errors++; /* Retain the in-flight request for the next bounded retry. */
#else
    bool ready = gpio_get_level(TC_ESP_READY);
    bool pending = (tc_pop(&out, tx, now, 100) || tc_pop(&outlo, tx, now, 100));
    if (!pending && !ready)
        return;
    if (pending && i2c_master_transmit(device, tx, 128, 8) != ESP_OK)
        stats.errors++;
    if (ready) {
        if (i2c_master_receive(device, rx, 128, 8) == ESP_OK) {
            tc_frame f;
            if (tc_decode(rx, &f)) {
                accept(rx, now);
                uint8_t ack[12] = {'A', 'K', 2, 0};
                tc_put32(ack + 4, f.seq);
                tc_put32(ack + 8, tc_get32(rx + 124));
                if (i2c_master_transmit(device, ack, 12, 8) != ESP_OK)
                    stats.errors++;
            }
        } else
            stats.errors++;
    }
#endif
#endif
}
bool tc_bus_receive(uint8_t *b, uint32_t now) {
    return tc_pop(&hi, b, now, 100) || tc_pop(&lo, b, now, 100);
}
void tc_bus_get_stats(tc_bus_stats *s) {
    *s = stats;
}

void tc_bus_cancel_pending(void){
    out.head=out.used=outlo.head=outlo.used=0;
#if TC_BACKEND == TC_SPI
    spi_inflight=false;
#endif
}
