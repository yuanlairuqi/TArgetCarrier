#include "motion_profile.h"
#include "motor_config.h"
#include <string.h>
#ifndef MP_DIR_HOLD_MS
#define MP_DIR_HOLD_MS MOTOR_DIRECTION_HOLD_MS
#endif
#ifndef MP_DIR_SETUP_MS
#define MP_DIR_SETUP_MS MOTOR_DIRECTION_SETUP_MS
#endif
void mp_abort(motion_profile_t *m){memset(m,0,sizeof(*m));}
bool mp_command(motion_profile_t *m,uint8_t op,uint16_t hz,uint8_t mode,
                uint32_t forward,uint32_t reverse,uint16_t accel,uint16_t decel,uint32_t now){
 if(op>3 || mode>1 || !accel || !decel || (op>0&&op<3&&!hz) ||
    (mode && (forward<100 || reverse<100 || forward>3600000 || reverse>3600000)))return false;
 if(op==0){mp_abort(m);m->enabled=true;m->last_ms=now;return true;}
 bool same=m->enabled && m->requested==op && m->mode==mode &&
           m->forward_ms==forward && m->reverse_ms==reverse;
 bool speed_changed=m->target_hz!=hz;
 m->accel=accel;m->decel=decel;m->target_hz=hz;
 m->mode=mode;m->forward_ms=forward;m->reverse_ms=reverse;
 if(!m->enabled){m->enabled=true;m->last_ms=now;}
 if(op==3){
   m->requested=3;m->soft_stop=true;m->turnaround=false;
   if(m->milli_hz)m->phase=MP_DECEL;else m->phase=MP_IDLE;
   return true;
 }
 m->soft_stop=false;m->requested=op;
 if(m->phase==MP_IDLE){m->direction=op;m->phase=MP_DIR_HOLD;m->phase_ms=now;}
 else if(!same && m->direction!=op){m->next_direction=op;m->turnaround=true;m->phase=MP_DECEL;}
 else if((same || m->direction==op) && speed_changed && !m->turnaround && (m->phase==MP_CRUISE||m->phase==MP_ACCEL||m->phase==MP_DECEL)){
   m->phase=m->milli_hz<(uint64_t)hz*1000?MP_ACCEL:MP_DECEL;
 }
 return true;
}
void mp_tick(motion_profile_t *m,uint32_t now){
 if(!m->enabled)return;
 uint32_t dt=now-m->last_ms;m->last_ms=now;
 uint64_t target=(uint64_t)m->target_hz*1000U;
 switch(m->phase){
 case MP_DIR_HOLD:
   if((uint32_t)(now-m->phase_ms)>=MP_DIR_HOLD_MS){
     m->phase=MP_DIR_SETUP;m->phase_ms=now;
   }break;
 case MP_DIR_SETUP:
   if((uint32_t)(now-m->phase_ms)>=MP_DIR_SETUP_MS){m->phase=MP_ACCEL;m->last_ms=now;}
   break;
 case MP_ACCEL:{
   uint64_t step=(uint64_t)m->accel*dt;
   if(m->milli_hz>=target || step>=target-m->milli_hz){m->milli_hz=target;m->phase=MP_CRUISE;m->phase_ms=now;}
   else m->milli_hz+=step;
   break;}
 case MP_CRUISE:
   if(m->mode && (uint32_t)(now-m->phase_ms)>=(m->direction==1?m->forward_ms:m->reverse_ms)){
     m->next_direction=(uint8_t)(m->direction==1?2:1);m->turnaround=true;m->phase=MP_DECEL; /* The next tick begins deceleration; cruise time alone is counted. */
   }break;
 case MP_DECEL:{
   bool reverse=m->turnaround;
   /* A reduction of target speed in one direction stops at the new target;
    * a cycle boundary, reversal or soft STOP ramps all the way to zero. */
   bool boundary=m->soft_stop || reverse;
   uint64_t goal=boundary?0:target,step=(uint64_t)m->decel*dt;
   if(m->milli_hz<=goal || step>=m->milli_hz-goal){
     m->milli_hz=goal;
     if(goal){m->phase=MP_CRUISE;m->phase_ms=now;}
     else if(m->soft_stop){m->phase=MP_IDLE;}
     else{
       m->direction=m->next_direction;
       m->turnaround=false;m->phase=MP_DIR_HOLD;m->phase_ms=now;
     }
   }else m->milli_hz-=step;
   break;}
 default:break;
 }
}
