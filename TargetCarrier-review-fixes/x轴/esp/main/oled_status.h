#pragma once
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    uint32_t uptime, pulse_hz;
    uint16_t current_hz,target_hz;uint8_t stage;
    uint8_t motion, speed, error, source, stm_motion, stm_error, direction;
    bool ble, stm_online, confirmed, capture_available, pulse_recent;
} oled_status_t;
void oled_status_publish(const oled_status_t *s);
void oled_status_start(void);
