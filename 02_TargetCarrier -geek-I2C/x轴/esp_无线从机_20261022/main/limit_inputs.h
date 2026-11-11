#ifndef LIMIT_INPUTS_H
#define LIMIT_INPUTS_H
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* G1/G2 inputs currently disabled in limit_inputs.c (#if 0).
 * Original active-low OR latch implementation is retained for restoration. */
bool limit_inputs_init(TaskHandle_t waiter);
bool limit_inputs_blocked(void);
void limit_inputs_sample(uint32_t now);
bool limit_inputs_try_reset(uint32_t now);
#endif
