#ifndef OLED_CONFIG_H
#define OLED_CONFIG_H
#ifndef APP_OLED_SH1106
#define APP_OLED_SH1106 0
#endif
#ifndef APP_OLED_HEIGHT
#define APP_OLED_HEIGHT 64
#endif
#ifndef APP_OLED_ADDRESS
#define APP_OLED_ADDRESS 0 /* 0 probes 0x3c and 0x3d; 7-bit, never 0x78. */
#endif
#ifndef APP_OLED_PERIOD_MS
#define APP_OLED_PERIOD_MS 500U
#endif
#if APP_OLED_HEIGHT != 32 && APP_OLED_HEIGHT != 64
#error OLED height must be 32 or 64
#endif
#if APP_OLED_ADDRESS != 0 && APP_OLED_ADDRESS != 0x3c && APP_OLED_ADDRESS != 0x3d
#error OLED address must be 0, 0x3c or 0x3d
#endif
#if APP_OLED_PERIOD_MS < 250 || APP_OLED_PERIOD_MS > 5000
#error OLED refresh period must be 250..5000 ms
#endif
#endif
