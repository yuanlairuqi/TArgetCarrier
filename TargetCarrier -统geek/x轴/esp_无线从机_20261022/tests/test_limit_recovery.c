#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "robot_control.h"
#include "robot_bridge.h"
#include "control_feedback.h"
#include "now_protocol.h"
static bool released;
static unsigned reset_calls;
static bool reset(void *ctx){(void)ctx;++reset_calls;return released;}
static void cmd(uint8_t d[16],unsigned version,unsigned motion,uint32_t seq,bool reset_request){
    memset(d,0,16);d[0]=(uint8_t)version;d[1]=(uint8_t)motion;d[2]=motion?40:0;
    wire_put32(d+4,seq);
    if(version==4){unsigned a=reset_request?ROBOT_RESET_ACCEL:200,dec=reset_request?ROBOT_RESET_DECEL:200;
        d[12]=(uint8_t)a;d[13]=(uint8_t)(a>>8);d[14]=(uint8_t)dec;d[15]=(uint8_t)(dec>>8);}
}
int main(void){
    uint8_t d[16],report[20];robot_control_t r;
    robot_init(&r);robot_connection(&r,true);robot_latch_limit(&r);r.reset_limit=reset;
    cmd(d,4,0,1,false);assert(robot_command(&r,d,16,10));
    assert(r.ack==1 && r.protocol==4 && r.error==10 && r.limit_latched && !robot_can_move(&r));
    cmd(d,4,1,2,false);assert(!robot_command(&r,d,16,11));assert(r.ack==1);
    cmd(d,4,0,2,true);released=false;assert(robot_command(&r,d,16,12));
    assert(r.ack==2 && r.limit_latched && r.error==10 && r.motion==0);
    released=true;assert(!robot_command(&r,d,16,13));assert(r.limit_latched); /* no replay reset */
    cmd(d,4,0,3,true);assert(robot_command(&r,d,16,14));
    assert(!r.limit_latched && r.error==0 && r.motion==0 && r.speed==0 && r.ack==3);
    assert(reset_calls==2);puts("PASS fresh STOP/reset ACKs, rejected reset, replay and no auto motion");

    robot_bridge_t b={0};r.uart_configured=true;r.ack=5;r.revision=7;
    b.enabled=b.online=b.confirmed=true;b.revision=7;b.completed_phone=5;b.peer_flags=PEER_APPLIED;b.last_rx=100;
    assert(control_limit_reset_ready(&r,&b,101));
    b.waiting=true;assert(!control_limit_reset_ready(&r,&b,101));b.waiting=false;
    b.revision=6;assert(!control_limit_reset_ready(&r,&b,101));b.revision=7;
    b.completed_phone=4;assert(!control_limit_reset_ready(&r,&b,101));b.completed_phone=5;
    assert(!control_limit_reset_ready(&r,&b,700));
    b.peer_flags|=PEER_DEMO;assert(!control_limit_reset_ready(&r,&b,101));b.peer_flags=PEER_APPLIED;
    b.peer_flags|=PEER_LOCAL_BUSY;assert(!control_limit_reset_ready(&r,&b,101));b.peer_flags=PEER_APPLIED;
    b.current_hz=1;assert(!control_limit_reset_ready(&r,&b,101));b.current_hz=0;
    b.peer_phase=3;assert(!control_limit_reset_ready(&r,&b,101));b.peer_phase=0;
    b.peer_error=1;assert(!control_limit_reset_ready(&r,&b,101));b.peer_error=0;
    b.error=BRIDGE_ACK_TIMEOUT;assert(!control_limit_reset_ready(&r,&b,101));
    puts("PASS reset gate rejects stale, wrong revision/ACK, demo, moving and failed feedback");

    now_control_t n;now_control_init(&n,1,42);robot_init(&r);r.uart_configured=true;
    now_request_t q={.kind=NOW_COMMAND,.node=1,.boot=42,.epoch=1,.session=99,.seq=1};
    cmd(q.command,4,0,1,false);assert(now_control_command(&n,&r,&q,0,0)==NOW_OK);
    uint32_t epoch=n.epoch;robot_latch_limit(&r);now_control_tick(&n,&r,10);
    assert(n.active && n.epoch==epoch);
    q.seq=2;cmd(q.command,4,0,2,false);assert(now_control_command(&n,&r,&q,400,400)==NOW_OK);
    now_control_tick(&n,&r,900);assert(n.active && r.error==10 && r.ack==2);
    q.seq=3;cmd(q.command,4,1,3,false);assert(now_control_command(&n,&r,&q,901,901)==NOW_OK);
    assert(n.active && n.epoch==epoch && r.motion==0 && r.limit_latched);
    q.seq=4;cmd(q.command,4,0,4,true);r.reset_limit=reset;released=true;
    assert(now_control_command(&n,&r,&q,902,902)==NOW_OK);
    robot_status(&r,902,report);assert(report[7]==0 && report[5]==0 && r.motion==0);
    now_control_tick(&n,&r,1702);assert(!n.active); /* true silence still revokes */
    puts("PASS ESP-NOW limit preserves session, STOP keepalive, reset and real lease expiry");
    return 0;
}
