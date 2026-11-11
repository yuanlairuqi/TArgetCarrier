/* Legacy public API names retained for OLED/RTOS compatibility. No UART data path. */
#include "robot_uart.h"
#include "robot_peer.h"
#include "motor_motion.h"
#include "wl_stm.h"
static bool initialized;static robot_peer_t peer;static uint32_t last_rx,last_status,last_fault;
static uint8_t pending,status_bytes[WIRE_SIZE];volatile RobotUARTStatus g_robot_uart_status;
static bool Profile(void *ctx,uint8_t op,uint8_t speed,uint8_t mode,uint32_t forward,uint32_t reverse,uint16_t accel,uint16_t decel){
 (void)ctx;return Motor_ProfileCommand(op,speed,mode,forward,reverse,accel,decel);
}
static void ProfileStatus(void *ctx,uint8_t *direction,uint8_t *phase,uint16_t *hz,uint16_t *target,bool *active){
 (void)ctx;MotorProfileStatus s;Motor_ProfileStatus(&s);
 *direction=s.direction;*phase=s.phase;*hz=s.hz;*target=s.target_hz;*active=s.active;
}
static bool Ready(void *ctx)
{
  (void)ctx;
  return Motor_ContinuousReady();
}
static bool Apply(void *ctx, uint8_t motion, uint8_t speed)
{
  (void)ctx;
  return Motor_SetContinuous(motion, speed);
}
static void Stop(void *ctx) { (void)ctx; Motor_Stop(); }
static bool Stopped(void *ctx) { (void)ctx; return Motor_OutputStopped(); }
static bool Active(void *ctx) { (void)ctx; return Motor_OutputActive(); }
static bool LocalBusy(void *ctx)
{
  (void)ctx;
  return Motor_LocalBusy();
}


static void QueueStatus(wire_frame_t*f){if(Motor_HasFault()){f->payload[3]=PEER_DRIVER_ERROR;f->payload[2]&=(uint8_t)~PEER_APPLIED;}wire_encode(f,status_bytes);pending=1;}
void RobotUART_Init(UART_HandleTypeDef*unused){(void)unused;if(initialized)return;initialized=wl_stm_init();peer_hooks_t h={.profile=Profile,.profile_status=ProfileStatus,.ctx=0,.ready=Ready,.apply=Apply,.stop=Stop,.battery=0,.stopped=Stopped,.local_busy=LocalBusy,.active=Active};peer_init(&peer,true,&h);last_rx=last_status=HAL_GetTick();}
void RobotUART_Poll(void){if(!initialized)return;uint32_t now=HAL_GetTick();wl_stm_poll();wire_frame_t f,reply;uint32_t faults=wl_stm_errors();if(faults!=last_fault){last_fault=faults;++g_robot_uart_status.rx_faults;peer_force_stop(&peer,PEER_BAD_COMMAND);g_robot_uart_status.linked=0;}uint8_t d[WIRE_SIZE];for(int i=0;i<4;i++){unsigned n=wl_stm_read(d,sizeof(d));if(!n)break;if(n!=WIRE_SIZE||!wire_decode(d,&f)){++g_robot_uart_status.rx_faults;peer_force_stop(&peer,PEER_BAD_COMMAND);continue;}++g_robot_uart_status.frames_received;if(peer_receive(&peer,&f,now,&reply)){last_rx=now;g_robot_uart_status.linked=1;QueueStatus(&reply);}}
 if(g_robot_uart_status.linked&&now-last_rx>=ROBOT_UART_LINK_TIMEOUT_MS){g_robot_uart_status.linked=0;++g_robot_uart_status.link_timeouts;peer_force_stop(&peer,PEER_TIMEOUT);peer_status(&peer,&reply);QueueStatus(&reply);}peer_tick(&peer,now);if(peer.has_session&&now-last_status>=ROBOT_UART_STATUS_MS){peer_status(&peer,&reply);QueueStatus(&reply);last_status=now;}if(pending&&wl_stm_bound()){if(wl_stm_send(status_bytes,WIRE_SIZE)){pending=0;++g_robot_uart_status.frames_sent;}else{++g_robot_uart_status.tx_faults;peer_force_stop(&peer,PEER_DRIVER_ERROR);}}}
