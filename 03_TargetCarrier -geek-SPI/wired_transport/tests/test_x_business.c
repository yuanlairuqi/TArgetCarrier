
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "robot_uart.h"
#include "robot_wire.h"
#include "robot_peer.h"
#include "robot_bridge.h"
#include "motor_motion.h"
#include "motor_config.h"
#include "wl_stm.h"
#include "wl_codec.h"
TIM_TypeDef test_tim1;static uint32_t tick,mask;static wl_codec master,slave;
static uint8_t pending[128],feedback[32];static bool have_rx,have_feedback;static unsigned sent;
uint32_t HAL_GetTick(void){return tick;}
void HAL_GPIO_WritePin(void*p,uint32_t n,GPIO_PinState v){(void)p;(void)n;(void)v;}
uint32_t __get_PRIMASK(void){return mask;}void __disable_irq(void){mask=1;}void __set_PRIMASK(uint32_t p){mask=p;}
bool wl_stm_init(void){wl_codec_init(&slave,false,1,0);return true;}
bool wl_stm_bound(void){return slave.bound;}void wl_stm_poll(void){}
unsigned wl_stm_read(uint8_t*out,unsigned cap){if(!have_rx)return 0;have_rx=false;unsigned n;if(!wl_decode(&slave,pending,out,&n))return 0;assert(n<=cap);return n;}
bool wl_stm_send(const uint8_t*d,unsigned n){uint8_t b[128];unsigned got;if(!wl_encode(&slave,d,n,false,tick,b))return false;assert(wl_decode(&master,b,feedback,&got)&&got==32);have_feedback=true;sent++;return true;}
uint32_t wl_stm_errors(void){return slave.rejected;}
static void Frame(const wire_frame_t*f,bool safe){uint8_t d[32];wire_encode(f,d);assert(wl_encode(&master,d,32,safe,tick,pending));have_rx=true;have_feedback=false;RobotUART_Poll();}
static wire_frame_t Reply(void){wire_frame_t r;assert(have_feedback&&wire_decode(feedback,&r));return r;}
static void Advance(unsigned n){while(n--){tick++;RobotUART_Poll();Motor_Process();}}
int main(void){UART_HandleTypeDef unused={USART3,0};TIM_HandleTypeDef timer={TIM1};test_tim1.CCMR1=TIM_OCMODE_PWM2;wl_codec_init(&master,true,1,0x12345678);RobotUART_Init(&unused);Motor_StartOnce(&timer);Motor_Stop();assert(Motor_OutputStopped());robot_bridge_t bridge;wire_frame_t f,r;bridge_init(&bridge,true,42);assert(bridge_tick(&bridge,tick,&f));Frame(&f,true);r=Reply();assert(bridge_receive(&bridge,&r,tick)&&bridge.confirmed&&(bridge.peer_flags&PEER_APPLIED));
 bridge_request(&bridge,1,1,40,101,tick);assert(bridge_tick(&bridge,tick,&f));Frame(&f,false);r=Reply();assert(bridge_receive(&bridge,&r,tick));Advance(15);assert(Motor_OutputActive());uint8_t old[128];memcpy(old,pending,128);
 bridge_request(&bridge,2,0,0,102,tick);assert(bridge_tick(&bridge,tick,&f));Frame(&f,true);r=Reply();assert(bridge_receive(&bridge,&r,tick));assert(Motor_OutputStopped());
 memcpy(pending,old,128);have_rx=true;RobotUART_Poll();assert(Motor_OutputStopped());Advance(700);assert(!g_robot_uart_status.linked&&Motor_OutputStopped());memcpy(pending,old,128);have_rx=true;RobotUART_Poll();assert(Motor_OutputStopped());
 wl_codec_init(&master,true,1,77);wire_frame_t move={.type=WIRE_COMMAND,.session=77,.token=1};move.payload[0]=1;move.payload[1]=40;move.payload[2]=0xf4;move.payload[3]=1;Frame(&move,false);assert(Motor_OutputStopped());wire_frame_t stop={.type=WIRE_COMMAND,.session=77,.token=2};stop.payload[2]=0xf4;stop.payload[3]=1;Frame(&stop,true);assert(Motor_OutputStopped());r=Reply();assert(r.session==77&&r.payload[0]==0&&(r.payload[2]&PEER_APPLIED));
 assert(sent>=3);puts("PASS production ESP bridge + new STM wired adapter + motor model: STOP handshake, explicit start/stop, old packet, timeout, new owner STOP-first");return 0;}
