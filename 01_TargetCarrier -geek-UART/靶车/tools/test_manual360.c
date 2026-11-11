
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "protocol.h"
#include "config.h"
#include "gimbal_motion.h"
#include "dm_motor.h"
#include "ble_guard.h"
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);int rates[]={0,90,180,360,-90,-180,-360};
 for(int i=0;i<7;i++){
  uint8_t raw[17];assert(fread(raw,1,17,f)==17);assert(tb_frame_valid(raw,17));
  ctrl_payload_t p;uint8_t seq;assert(proto_decode_ctrl(raw,17,&p,&seq));assert(p.yawrate_q==rates[i]*10);
  float dps=proto_q_to_dps(p.yawrate_q);assert(fabsf(dps-rates[i])<.001f);
  gm_motion_t m={0};for(int j=0;j<26000;j++){
   float prev=m.velocity;
   gm_step(&m,true,true,0,dps*3.14159265359f/180,0,.001f,GIMBAL_MANUAL_SPEED_MAX_DPS*3.14159265359f/180,GIMBAL_ACCEL_DPS2*3.14159265359f/180,GIMBAL_JERK_DPS3*3.14159265359f/180,MOTOR_PMAX);
   assert(fabsf(m.velocity-prev)<=GIMBAL_ACCEL_DPS2*3.14159265359f/180*.001f+0.00001f);
  }
  assert(fabsf(m.velocity-dps*3.14159265359f/180)<.0001f);
  dm_mit_cmd_t cmd={.vel_rad_s=m.velocity};uint8_t can[8];dm_pack_mit(1,&cmd,can);
  unsigned q=(can[2]<<4)|(can[3]>>4);float decoded=dm_uint_to_float(q,-MOTOR_VMAX,MOTOR_VMAX,12);
  assert(fabsf(decoded-m.velocity)<=2*MOTOR_VMAX/4095+.00001f);
  gm_step(&m,false,true,0,0,0,.001f,1,1,1,MOTOR_PMAX);assert(!m.active && m.velocity==0);
 }
 fclose(f);
 uint8_t bad[17]={0};ctrl_target_t t={0};t.yaw_rate_dps=360;proto_encode_ctrl(bad,&t,CTRL_FLAG_VELOCITY,0,7);
 /* Valid CRC and out-of-range raw rate must be refused by the ESP guard. */
 bad[11]=0x11;bad[12]=0x0e; /* 360.1 dps = 3601 */
 /* Use the encoder CRC algorithm after modifying the wire payload. */
 uint16_t crc=proto_crc16(bad+1,14);bad[15]=(uint8_t)crc;
 assert(!tb_frame_valid(bad,17));
 assert(GIMBAL_SPEED_MAX_DPS==90 && GIMBAL_MANUAL_SPEED_MAX_DPS==360);
 puts("PASS PC bytes -> ESP guard -> STM decode/ramp -> MIT CAN; all seven setpoints, bounded acceleration, STOP and legacy trajectory limit");
}
