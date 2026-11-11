#pragma once
#include "robot_control.h"
#include "robot_bridge.h"
#include <string.h>
typedef struct { uint8_t bytes[20]; bool fault_active,have_fault; } diagnostic_state_t;
static inline void diagnostic_update(diagnostic_state_t *s,const robot_control_t *r,const robot_bridge_t *b,uint32_t now,uint32_t loop_max,uint8_t notify_error,uint8_t reset_reason){
 uint8_t error=r->error?r->error:b->error;
 if(error && s->fault_active)return; /* Preserve the first raw cause, not a later STOP reply. */
 if(!error){s->fault_active=false;if(s->have_fault)return;}
 uint8_t *p=s->bytes;memset(p,0,20);p[0]=1;p[1]=r->error;p[2]=b->error;p[3]=b->peer_error;
 p[4]=b->peer_flags;p[5]=b->peer_phase;p[6]=r->motion;p[7]=r->speed;
 for(int i=0;i<4;i++){p[8+i]=(uint8_t)(now>>(8*i));p[12+i]=(uint8_t)(loop_max>>(8*i));}
 uint32_t age=b->online?(uint32_t)(now-b->last_rx):65535U;if(age>65535U)age=65535U;
 p[16]=(uint8_t)age;p[17]=(uint8_t)(age>>8);p[18]=notify_error;p[19]=reset_reason;
 if(error){s->have_fault=true;s->fault_active=true;}
}
