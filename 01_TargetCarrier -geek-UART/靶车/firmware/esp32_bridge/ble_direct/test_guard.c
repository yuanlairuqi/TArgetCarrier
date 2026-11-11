#include <assert.h>
#include <stdio.h>
#include "ble_guard.h"
static void ctrl(uint8_t f[17],uint8_t seq,uint16_t flags){ctrl_target_t t={0};proto_encode_ctrl(f,&t,flags,MOTOR_MODE_MIT,seq);}
int main(void){
    tb_guard_t g;uint8_t f[17];tb_guard_init(&g);tb_guard_connection(&g,true);
    ctrl(f,1,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,7));assert(!tb_guard_accept(&g,f,0,true));
    ctrl(f,2,CTRL_FLAG_ESTOP);assert(tb_guard_accept(&g,f,1,false));assert(g.disarm_seen&&!g.active);
    ctrl(f,3,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,7));assert(!tb_guard_accept(&g,f,2,false));
    ctrl(f,4,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,7));assert(tb_guard_accept(&g,f,3,true));assert(g.active);
    assert(!tb_guard_accept(&g,f,4,true)); /* duplicate */
    assert(tb_guard_tick(&g,4,false));assert(g.failsafe&&!g.active);
    ctrl(f,5,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,7));assert(!tb_guard_accept(&g,f,5,true));
    ctrl(f,6,0);assert(tb_guard_accept(&g,f,6,true));
    ctrl(f,7,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,7));assert(!tb_guard_accept(&g,f,7,true));
    ctrl(f,8,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,8));assert(tb_guard_accept(&g,f,8,true));
    assert(tb_guard_tick(&g,608,true));assert(!g.active&&g.failsafe);
    tb_guard_connection(&g,false);tb_guard_connection(&g,true);
    ctrl(f,9,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,9));assert(!tb_guard_accept(&g,f,609,true));
    ctrl(f,10,CTRL_FLAG_ESTOP);assert(tb_guard_accept(&g,f,610,true));
    ctrl(f,11,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,9));assert(tb_guard_accept(&g,f,611,true));
    ctrl(f,12,CTRL_FLAG_ESTOP);assert(tb_guard_accept(&g,f,612,true));assert(!g.active);
    f[15]^=1;assert(!tb_frame_valid(f,17));
    proto_encode_gains(f,200,3,1);assert(tb_frame_valid(f,17));
    proto_encode_gains(f,201,3,1);assert(!tb_frame_valid(f,17));
    puts("PASS target BLE: disarm-before-arm, STM freshness, duplicate/old token, timeout, reconnect, stop and CRC/gains bounds");
    return 0;
}
