#pragma once
#include <stdbool.h>
#include <stdint.h>
void pulse_capture_init(void);
// Called by the telemetry task only. Capture does not generate motor commands.
void pulse_capture_poll(uint16_t connection,uint16_t characteristic,bool subscribed);
