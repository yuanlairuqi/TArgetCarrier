#pragma once
#include <stdbool.h>
#include <stdint.h>
void pulse_capture_init(void);
// Called by the telemetry task only. Capture does not generate motor commands.
void pulse_capture_poll(uint16_t connection,uint16_t characteristic,bool subscribed);

typedef struct {
    bool available, recent;
    uint32_t hz;
    uint8_t direction;
} pulse_capture_status_t;
void pulse_capture_status(pulse_capture_status_t *out);
