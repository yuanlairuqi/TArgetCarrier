#ifndef OLED_TEXT_H
#define OLED_TEXT_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define OLED_TEXT_COLS 21U
#define OLED_TEXT_ROWS 8U
typedef bool (*oled_write_fn)(void *ctx, uint8_t address, const uint8_t *bytes, size_t size);
typedef struct {
    oled_write_fn write;
    void *ctx;
    uint8_t address, height, column_offset;
    bool sh1106;
} oled_text_t;
bool oled_text_init(const oled_text_t *d);
bool oled_text_frame(const oled_text_t *d, const char lines[8][22], unsigned bank);
#endif
