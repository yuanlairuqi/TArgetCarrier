#ifndef GIMBAL_MOTION_H
#define GIMBAL_MOTION_H
/* MIT velocity control, independent of position wrap; no position limits.
 * Position input is a single-turn orientation and takes the nearest equivalent
 * representable MIT position. PMAX is a wire mapping bound, not a travel limit. */
#include <math.h>
#include <stdbool.h>
typedef struct { float position, velocity, acceleration; bool active, velocity_mode; } gm_motion_t;
static inline float gm_clip(float x,float lo,float hi){return fmaxf(lo,fminf(hi,x));}
static inline float gm_nearest(float angle,float reference,float pmax){
    const float turn=6.28318530718f;
    float p=angle+roundf((reference-angle)/turn)*turn;
    while(p>pmax)p-=turn;
    while(p< -pmax)p+=turn;
    return gm_clip(p,-pmax,pmax);
}
static inline void gm_reset(gm_motion_t *m,float feedback){
    m->position=feedback;m->velocity=0;m->acceleration=0;m->active=false;m->velocity_mode=false;
}
static inline void gm_step(gm_motion_t *m,bool enabled,bool velocity_mode,
                          float position,float speed,float feedback,float dt,
                          float vmax,float accel,float jerk,float pmax){
    if(!enabled){gm_reset(m,feedback);return;}
    if(!m->active || m->velocity_mode!=velocity_mode){
        /* Never inherit a stale position trajectory when changing submode. */
        m->position=feedback;
        if(!m->active){m->velocity=0;m->acceleration=0;}
        m->active=true;m->velocity_mode=velocity_mode;
    }
    dt=gm_clip(dt,0,.01f);
    float goal=velocity_mode?m->position:gm_nearest(position,m->position,pmax);
    float desired=velocity_mode?speed:gm_clip(speed+4.0f*(goal-m->position),-vmax,vmax);
    desired=gm_clip(desired,-vmax,vmax);
    /* Bounded acceleration, with jerk-smoothed onset. Landing at the requested
     * speed resets acceleration; this is not a strict global jerk guarantee. */
    float wanted=gm_clip((desired-m->velocity)/fmaxf(dt,.000001f),-accel,accel);
    m->acceleration+=gm_clip(wanted-m->acceleration,-jerk*dt,jerk*dt);
    float next=m->velocity+m->acceleration*dt;
    if((desired-m->velocity)*(desired-next)<=0){next=desired;m->acceleration=0;}
    m->velocity=gm_clip(next,-vmax,vmax);
    if(!velocity_mode)m->position=gm_clip(m->position+m->velocity*dt,-pmax,pmax);
    /* In velocity mode position does not accumulate, even after hours. */
}
#endif
