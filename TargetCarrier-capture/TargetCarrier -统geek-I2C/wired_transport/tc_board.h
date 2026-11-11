#ifndef TC_BOARD_H
#define TC_BOARD_H
#define TC_UART 1
#define TC_SPI 2
#define TC_I2C 3
#define TC_BACKEND 3
#define WL_BACKEND_LABEL "I2C"
/* M5Stack Stamp-S3Bat S015: accessible G1..G11 pads only.
 * G1/G2 limits, G6/G7 capture, G8/G9 OLED, G10 user output are reserved.
 * Both receiver nodes use this map; GEEK is a different board. */
#define TC_ESP_TX 4
#define TC_ESP_RX 5
#define TC_ESP_SCK 3
#define TC_ESP_MOSI 4
#define TC_ESP_MISO 5
#define TC_ESP_CS 10
#define TC_ESP_READY 11
#define TC_ESP_SDA 4
#define TC_ESP_SCL 5
#define TC_UART_BAUD 460800
#define TC_SPI_HZ 1000000
#define TC_I2C_HZ 400000
#define TC_I2C_ADDR 0x42
#define TC_QUEUE_AGE_MS 100
#define TC_POLL_MS 2
#define TC_WAVE_PERIOD_MS 40

/* Enforce the physical breakout and existing peripheral reservations. */
#define TC_STAMP_BUS_GPIO_OK(p) ((p)>=3 && (p)<=11 && ((p)<6 || (p)>10))
#if TC_BACKEND == TC_SPI
#if !TC_STAMP_BUS_GPIO_OK(TC_ESP_SCK) || !TC_STAMP_BUS_GPIO_OK(TC_ESP_MOSI) || !TC_STAMP_BUS_GPIO_OK(TC_ESP_MISO) || !TC_STAMP_BUS_GPIO_OK(TC_ESP_CS) || !TC_STAMP_BUS_GPIO_OK(TC_ESP_READY)
#error "Stamp-S3Bat SPI pin unavailable or reserved"
#endif
#if TC_ESP_SCK==TC_ESP_MOSI || TC_ESP_SCK==TC_ESP_MISO || TC_ESP_SCK==TC_ESP_CS || TC_ESP_SCK==TC_ESP_READY || TC_ESP_MOSI==TC_ESP_MISO || TC_ESP_MOSI==TC_ESP_CS || TC_ESP_MOSI==TC_ESP_READY || TC_ESP_MISO==TC_ESP_CS || TC_ESP_MISO==TC_ESP_READY || TC_ESP_CS==TC_ESP_READY
#error "Stamp-S3Bat duplicate SPI pin"
#endif
#elif TC_BACKEND == TC_I2C
#if !TC_STAMP_BUS_GPIO_OK(TC_ESP_SDA) || !TC_STAMP_BUS_GPIO_OK(TC_ESP_SCL) || !TC_STAMP_BUS_GPIO_OK(TC_ESP_READY)
#error "Stamp-S3Bat I2C pin unavailable or reserved"
#endif
#if TC_ESP_SDA==TC_ESP_SCL || TC_ESP_SDA==TC_ESP_READY || TC_ESP_SCL==TC_ESP_READY
#error "Stamp-S3Bat duplicate I2C pin"
#endif
#endif
#endif
