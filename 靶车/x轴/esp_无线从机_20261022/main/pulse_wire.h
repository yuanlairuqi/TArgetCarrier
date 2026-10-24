#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define PULSE_MAX_EDGES 512U
#define PULSE_PACKET_SIZE 20U
#define PULSE_AVAILABLE 1U
#define PULSE_OVERFLOW 2U
#define PULSE_EDGE_LOSS 4U
typedef struct { uint32_t tick; uint8_t tag; } pulse_edge_t;
typedef struct {
    uint16_t id, count;
    uint8_t flags, initial, pul_gpio, dir_gpio;
    uint32_t hz, duration;
    pulse_edge_t edges[PULSE_MAX_EDGES];
} pulse_snapshot_t;
void pulse_finalize(pulse_snapshot_t *s);
size_t pulse_packet_count(const pulse_snapshot_t *s);
bool pulse_packet(const pulse_snapshot_t *s, size_t index, uint8_t out[PULSE_PACKET_SIZE]);
