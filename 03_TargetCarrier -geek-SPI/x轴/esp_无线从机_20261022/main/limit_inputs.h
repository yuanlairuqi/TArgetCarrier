#ifndef LIMIT_INPUTS_H
#define LIMIT_INPUTS_H
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
/* Active-low GPIO1 OR GPIO2. Explicit reset requires both HIGH for 100 ms. */
bool limit_inputs_init(TaskHandle_t waiter);
bool limit_inputs_blocked(void);
void limit_inputs_sample(uint32_t now);
bool limit_inputs_try_reset(uint32_t now);
#endif
