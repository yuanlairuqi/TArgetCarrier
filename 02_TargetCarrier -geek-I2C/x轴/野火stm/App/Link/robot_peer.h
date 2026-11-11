#ifndef ROBOT_PEER_H
#define ROBOT_PEER_H
#include "robot_wire.h"
enum { PEER_OK, PEER_BAD_COMMAND, PEER_TIMEOUT, PEER_NOT_READY, PEER_DRIVER_ERROR };
typedef struct {
    bool (*profile)(void *ctx,uint8_t op,uint8_t speed,uint8_t mode,uint32_t forward,uint32_t reverse,uint16_t accel,uint16_t decel);
    void (*profile_status)(void *ctx,uint8_t *direction,uint8_t *phase,uint16_t *hz,uint16_t *target,bool *active);
    void *ctx;
    bool (*ready)(void *ctx);
    bool (*apply)(void *ctx,uint8_t motion,uint8_t speed);
    void (*stop)(void *ctx);
    bool (*battery)(void *ctx,uint16_t *mv);
    bool (*stopped)(void *ctx);
    bool (*local_busy)(void *ctx);
    bool (*active)(void *ctx); // True only after physical output generation has started.
} peer_hooks_t;
typedef struct {
    peer_hooks_t hooks;
    bool extended;
    bool hardware, has_session, has_token, applied;
    uint32_t session, token, phone_seq, lease_at;
    uint16_t lease_ms;
    uint8_t motion, speed, error, mode, requested_motion;
    uint32_t forward_ms, reverse_ms, phase_at;
} robot_peer_t;
void peer_init(robot_peer_t *p,bool hardware,const peer_hooks_t *hooks);
void peer_tick(robot_peer_t *p,uint32_t now);
bool peer_receive(robot_peer_t *p,const wire_frame_t *in,uint32_t now,wire_frame_t *reply);
void peer_status(robot_peer_t *p,wire_frame_t *out);
void peer_force_stop(robot_peer_t *p,uint8_t error);
#endif
