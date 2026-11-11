#ifndef TC_BOARD_H
#define TC_BOARD_H
#define TC_UART 1
#define TC_SPI 2
#define TC_I2C 3
#ifndef TC_BACKEND
#define TC_BACKEND TC_UART
#endif
#if TC_BACKEND < 1 || TC_BACKEND > 3
#error Select exactly one backend: 1 UART, 2 SPI, 3 I2C
#endif
#ifndef TC_ESP_TX
#define TC_ESP_TX 4
#define TC_ESP_RX 5
#define TC_ESP_SCK 12
#define TC_ESP_MOSI 11
#define TC_ESP_MISO 13
#define TC_ESP_CS 10
#define TC_ESP_READY 9
#define TC_ESP_SDA 8
#define TC_ESP_SCL 18
#endif
#define TC_UART_BAUD 460800
#define TC_SPI_HZ 1000000
#define TC_I2C_HZ 100000
#define TC_I2C_ADDR 0x42
#define TC_QUEUE_AGE_MS 100
#define TC_POLL_MS 10
#define TC_WAVE_PERIOD_MS 40
#endif
