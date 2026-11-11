#ifndef MOTION_PROFILE_H
#define MOTION_PROFILE_H
#include <stdbool.h>
#include <stdint.h>
enum {MP_IDLE,MP_ACCEL,MP_CRUISE,MP_DECEL,MP_DIR_HOLD,MP_DIR_SETUP,MP_FAULT};
typedef struct {
 uint64_t milli_hz;
 uint32_t last_ms,phase_ms,forward_ms,reverse_ms;
 uint16_t target_hz,accel,decel;
 uint8_t phase,direction,requested,mode,next_direction;
 bool enabled,soft_stop,turnaround;
} motion_profile_t;
bool mp_command(motion_profile_t *m,uint8_t op,uint16_t hz,uint8_t mode,
                uint32_t forward,uint32_t reverse,uint16_t accel,uint16_t decel,uint32_t now);
void mp_tick(motion_profile_t *m,uint32_t now);
void mp_abort(motion_profile_t *m);
#endif
