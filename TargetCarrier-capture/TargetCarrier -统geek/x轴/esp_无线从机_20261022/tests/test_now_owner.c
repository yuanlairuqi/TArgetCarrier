#include "now_protocol.h"
#include <assert.h>
#include <stdio.h>
int main(void){
 now_control_t n;robot_control_t r;robot_init(&r);now_control_init(&n,1,7);r.uart_configured=true;r.stm_online=true;r.stm_flags=1;
 now_ble_connection(&n,&r,true);uint8_t d[16]={4,0,0,0,1,0,0,0,0,0,0,0,200,0,200,0};
 now_request_t q={.kind=NOW_COMMAND,.node=1,.boot=7,.epoch=n.epoch,.session=9,.seq=1};
 for(int i=0;i<16;i++){q.command[i]=d[i];}
 assert(now_control_command(&n,&r,&q,1,1)==NOW_OK&&n.active);
 uint32_t epoch=n.epoch;now_ble_connection(&n,&r,false);now_ble_connection(&n,&r,true);assert(n.active&&n.epoch==epoch);
 d[1]=1;d[2]=40;assert(!now_ble_command(&n,&r,d,16,2));
 robot_latch_limit(&r);assert(r.limit_latched&&r.motion==0);now_control_tick(&n,&r,2);assert(n.active&&r.limit_latched);
 d[1]=0;d[2]=0;assert(now_ble_command(&n,&r,d,16,3));assert(!n.active&&r.motion==0&&r.limit_latched&&n.epoch!=epoch);
 assert(now_control_command(&n,&r,&q,4,4)==NOW_CHALLENGE);puts("PASS X GEEK ownership / BLE observer / BLE STOP / sticky local limit / old epoch");return 0;
}
