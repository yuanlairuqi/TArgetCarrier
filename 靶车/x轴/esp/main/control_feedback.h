#ifndef CONTROL_FEEDBACK_H
#define CONTROL_FEEDBACK_H
#include "robot_control.h"
#include "robot_bridge.h"
// A reused per-source sequence is not proof that the current revision reached STM32.
static inline void control_feedback(robot_control_t *r,const robot_bridge_t *b) {
    bool current=b->online&&b->confirmed&&!b->waiting&&b->revision==r->revision&&b->completed_phone==r->ack;
    r->current_hz=b->online?b->current_hz:0;r->target_hz=b->online?b->target_hz:0;
    r->stm_mode=b->peer_mode;r->stm_phase=b->peer_phase;r->stm_online=b->online;
    r->stm_motion=b->peer_motion;r->stm_error=b->peer_error;
    r->stm_flags=current?b->peer_flags:(uint8_t)(b->peer_flags&~PEER_APPLIED);
    r->stm_ack=current?b->ack:0;
}
#endif
