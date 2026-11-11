#ifndef WAVE_STORE_H
#define WAVE_STORE_H
#include <stdbool.h>
#include <stdint.h>
/* Nonblocking calls from USB/radio owner; all filesystem work belongs to worker. */
bool wave_store_init(void);
void wave_store_frame(const uint8_t body[64],uint64_t receive_us);
bool wave_store_request(const uint8_t request[64]);
bool wave_store_response(uint8_t response[64]);
#endif
