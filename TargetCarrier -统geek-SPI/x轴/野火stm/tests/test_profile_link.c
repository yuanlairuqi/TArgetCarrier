#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "motor_motion.h"
#include "motion_profile.h"
#include "robot_peer.h"
#include "robot_bridge.h"
#include "robot_control.h"
#include "control_feedback.h"
TIM_TypeDef test_tim1;
static uint32_t now,seq;
static uint32_t dir;
static robot_control_t robot;
static robot_bridge_t bridge;
static robot_peer_t peer;
uint32_t HAL_GetTick(void){return now;}
void HAL_GPIO_WritePin(void *p,uint32_t n,GPIO_PinState v){
 (void)p;(void)n;
 if(dir!=(uint32_t)v)assert(!(test_tim1.CR1&TIM_CR1_CEN));
 dir=v;
}
static bool ready(void *p){(void)p;return Motor_ContinuousReady();}
static bool apply(void *p,uint8_t op,uint8_t speed){(void)p;return Motor_SetContinuous(op,speed);}
static void stop(void *p){(void)p;Motor_Stop();}
static bool stopped(void *p){(void)p;return Motor_OutputStopped();}
static bool profile(void *p,uint8_t op,uint8_t speed,uint8_t mode,uint32_t f,uint32_t r,uint16_t a,uint16_t d){
 (void)p;return Motor_ProfileCommand(op,speed,mode,f,r,a,d);
}
static void snapshot(void *p,uint8_t *direction,uint8_t *phase,uint16_t *hz,uint16_t *target,bool *active){
 (void)p;MotorProfileStatus s;Motor_ProfileStatus(&s);*direction=s.direction;*phase=s.phase;*hz=s.hz;*target=s.target_hz;*active=s.active;
}
static void command(uint8_t op,uint8_t speed,uint8_t mode,uint16_t f,uint16_t r,uint16_t a,uint16_t d){
 uint8_t data[16]={4,op,speed,mode};wire_put32(data+4,++seq);
 data[8]=(uint8_t)f;data[9]=(uint8_t)(f>>8);data[10]=(uint8_t)r;data[11]=(uint8_t)(r>>8);
 data[12]=(uint8_t)a;data[13]=(uint8_t)(a>>8);data[14]=(uint8_t)d;data[15]=(uint8_t)(d>>8);
 if(!robot_command(&robot,data,16,now)){fprintf(stderr,"rejected seq=%u time=%u op=%u flags=%u stm_error=%u err=%u link=%u\n",seq,now,op,robot.stm_flags,robot.stm_error,robot.error,robot.link_error);assert(0);}
 bridge_profile(&bridge,robot.protocol,robot.accel,robot.decel);
 bridge_parameters(&bridge,robot.mode,robot.forward_ms,robot.reverse_ms);
 bridge_request(&bridge,robot.revision,robot.motion,robot.speed,robot.ack,now);
 wire_frame_t out,reply;assert(bridge_tick(&bridge,now,&out));
 assert(out.type==WIRE_PROFILE);
 uint8_t raw[WIRE_SIZE];wire_encode(&out,raw);wire_frame_t decoded;assert(wire_decode(raw,&decoded));
 assert(peer_receive(&peer,&decoded,now,&reply));assert(bridge_receive(&bridge,&reply,now));
 control_feedback(&robot,&bridge);assert(bridge.error==0);
 uint8_t status[20];robot_status(&robot,now,status);assert(status[0]==4);assert(status[5]==op);
}
static void step(unsigned ms){for(unsigned i=0;i<ms;i++){++now;peer_tick(&peer,now);Motor_Process();}}
static void run(unsigned ms,uint8_t op,uint8_t speed,uint8_t mode,uint16_t a,uint16_t d){
 for(unsigned t=0;t<ms;t+=100){command(op,speed,mode,mode?10:0,mode?10:0,a,d);step(ms-t<100?ms-t:100);}
}
int main(void){
 TIM_HandleTypeDef timer={TIM1};test_tim1.CCMR1=TIM_OCMODE_PWM2;
 Motor_StartOnce(&timer);Motor_Stop();
 peer_hooks_t hooks={.ready=ready,.apply=apply,.stop=stop,.stopped=stopped,.profile=profile,.profile_status=snapshot};
 peer_init(&peer,true,&hooks);robot_init(&robot);robot_connection(&robot,true);
 robot.uart_configured=true;robot.stm_online=true;robot.stm_flags=PEER_READY;
 bridge_init(&bridge,true,123);
 command(0,0,0,0,0,200,200);assert(Motor_OutputStopped());
 run(1500,1,100,0,200,200);MotorProfileStatus s;Motor_ProfileStatus(&s);
 assert(s.phase==MP_ACCEL && s.hz>250 && s.hz<310 && !bridge.waiting);
 puts("PASS real PC-v4/ESP control/bridge/UART/peer/motor path accepts long ramps before target speed");
 wire_frame_t stale={.type=WIRE_COMMAND,.session=124,.token=peer.token+1},reply;stale.payload[0]=1;stale.payload[1]=40;stale.payload[2]=244;stale.payload[3]=1;
 assert(!peer_receive(&peer,&stale,now,&reply));stale.session=123;stale.token=peer.token-1;assert(peer_receive(&peer,&stale,now,&reply));peer_status(&peer,&reply);assert(peer.extended && (reply.payload[2]&PEER_PROFILE));
 puts("PASS rejected legacy session cannot downgrade active v4 mode");
 run(4000,1,100,0,200,200);Motor_ProfileStatus(&s);assert(s.phase==MP_CRUISE && s.hz==1000);
 run(1500,3,0,0,200,100);Motor_ProfileStatus(&s);assert(s.phase==MP_DECEL && s.hz>=840 && s.hz<=860);
 puts("PASS repeated soft STOP heartbeats do not reset deceleration and preserve leases beyond one second");
 step(501);assert(Motor_OutputStopped());assert(peer.error==PEER_TIMEOUT);
 puts("PASS lease expiration during soft STOP immediately removes pulses");
 command(0,0,0,0,0,200,200);
 run(1600,1,100,0,1000,1000);Motor_ProfileStatus(&s);assert(s.hz==1000);
 run(100,2,100,0,1000,1000);Motor_ProfileStatus(&s);assert(s.direction==1 && s.phase==MP_DECEL);
 run(2000,2,100,0,1000,1000);Motor_ProfileStatus(&s);assert(s.direction==2 && s.phase==MP_CRUISE);
 puts("PASS reverse request decelerates to zero before DIR hold/setup and reverse acceleration");
 command(0,0,0,0,0,200,200);assert(Motor_OutputStopped());step(50);assert(Motor_OutputStopped());
 puts("PASS emergency STOP preempts the profile and cannot resume it");
 run(1500,1,1,0,1000,100);run(1600,3,0,0,1000,100);Motor_ProfileStatus(&s);assert(s.phase==MP_IDLE && s.hz==0 && Motor_OutputStopped());
 puts("PASS normal deceleration reaches zero while heartbeats continue");
 command(0,0,0,0,0,200,200);
 motion_profile_t m={0};assert(mp_command(&m,1,100,1,1000,1000,100,100,0));
 mp_tick(&m,2);mp_tick(&m,12);mp_tick(&m,1012);assert(m.phase==MP_CRUISE);
 mp_tick(&m,2011);assert(m.phase==MP_CRUISE);mp_tick(&m,2012);assert(m.phase==MP_DECEL);
 mp_tick(&m,3012);assert(m.milli_hz==0 && m.phase==MP_DIR_HOLD && m.direction==2);
 mp_tick(&m,3014);mp_tick(&m,3024);mp_tick(&m,4024);assert(m.phase==MP_CRUISE);
 assert(mp_command(&m,1,50,1,1000,1000,100,100,4024));mp_tick(&m,4524);
 assert(m.direction==2 && m.milli_hz==50000 && m.phase==MP_CRUISE);
 puts("PASS cruise-only cycle duration and online reduction in reverse phase retain the cycle direction");
 m=(motion_profile_t){0};assert(mp_command(&m,1,100,0,0,0,1,1,0xfffffffaU));
 mp_tick(&m,0xfffffffcU);mp_tick(&m,6);for(uint32_t i=7;i<=1006;i++)mp_tick(&m,i);
 assert(m.milli_hz==1000);
 puts("PASS fractional 1 Hz/s integration and uint32 time wrap");
 m=(motion_profile_t){0};assert(mp_command(&m,1,100,1,1000,1000,100,100,0));mp_tick(&m,2);mp_tick(&m,12);mp_tick(&m,1012);
 assert(mp_command(&m,1,50,0,0,0,100,100,1012));assert(m.phase==MP_DECEL);mp_tick(&m,1512);assert(m.phase==MP_CRUISE && m.milli_hz==50000);
 puts("PASS mode and speed update cannot leave the previous cruise frequency latched");
 return 0;
}
