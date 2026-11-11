#ifndef NOW_PROTOCOL_H
#define NOW_PROTOCOL_H
#include "robot_control.h"
#define NOW_FRAME_SIZE 64
#define NOW_QUEUE_MAX_AGE_MS 100
enum { NOW_QUERY=1, NOW_COMMAND=2, NOW_STATUS=3 };
enum { NOW_OK, NOW_BUSY, NOW_CHALLENGE, NOW_NEED_STOP, NOW_SESSION,
       NOW_REPLAY, NOW_BAD_COMMAND, NOW_STALE };
typedef struct {
    uint8_t kind;
    uint32_t node, boot, epoch, session, seq;
    uint8_t command[ROBOT_CMD_SIZE];
} now_request_t;
typedef struct {
    uint32_t node, boot, epoch, session, last_fresh_ms;
    bool active, ble_connected;
} now_control_t;
uint32_t now_crc32(const uint8_t *p, size_t n);
bool now_decode(const uint8_t *data, size_t n, now_request_t *out);
void now_control_init(now_control_t *n, uint32_t node, uint32_t boot);
void now_ble_connection(now_control_t *n, robot_control_t *r, bool connected);
void now_control_tick(now_control_t *n, robot_control_t *r, uint32_t now);
uint8_t now_control_command(now_control_t *n, robot_control_t *r,
                          const now_request_t *q, uint32_t received, uint32_t now);
void now_status_frame(const now_control_t *n, const robot_control_t *r,
                      const now_request_t *q, uint8_t result, uint32_t now,
                      uint8_t out[NOW_FRAME_SIZE]);
#endif
