#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "robot_control.h"
#include "robot_bridge.h"
#include "robot_peer.h"
static unsigned stops;
static void stop(void *ctx){(void)ctx;++stops;}
static bool ready(void *ctx){(void)ctx;return true;}
static bool apply(void *ctx,uint8_t op,uint8_t speed){(void)ctx;(void)op;(void)speed;return true;}
static bool profile(void *ctx,uint8_t op,uint8_t speed,uint8_t mode,uint32_t f,uint32_t r,uint16_t a,uint16_t d){
    (void)ctx;(void)speed;(void)mode;(void)f;(void)r;(void)a;(void)d;
    assert(op==0);++stops;return true;
}
static void status(void *ctx,uint8_t *dir,uint8_t *phase,uint16_t *hz,uint16_t *target,bool *active){
    (void)ctx;*dir=0;*phase=0;*hz=0;*target=0;*active=false;
}
int main(void){
    for(unsigned protocol=3;protocol<=4;protocol++){
        robot_control_t r;robot_init(&r);robot_connection(&r,true);r.protocol=protocol;
        robot_bridge_t b;bridge_init(&b,true,123);
        robot_peer_t p;peer_hooks_t hooks={.stop=stop,.ready=ready,.apply=apply,.stopped=ready,.profile=profile,.profile_status=status};
        peer_init(&p,true,&hooks);wire_frame_t out,reply;
        assert(bridge_tick(&b,0,&out));assert(peer_receive(&p,&out,0,&reply));assert(bridge_receive(&b,&reply,0));
        /* Existing movement awaiting ACK, plus a queued reversal/cycle. */
        r.motion=1;r.speed=50;r.mode=1;r.forward_ms=1000;r.reverse_ms=1000;++r.revision;
        bridge_profile(&b,protocol,200,200);bridge_parameters(&b,1,1000,1000);
        bridge_request(&b,r.revision,1,50,1,10);assert(bridge_tick(&b,10,&out));
        ++r.revision;bridge_request(&b,r.revision,2,50,2,11);assert(b.queued);
        robot_latch_limit(&r);assert(r.motion==0 && !r.speed && !r.mode && !r.forward_ms && !r.reverse_ms);
        assert(!robot_can_move(&r));uint32_t revision=r.revision;
        robot_latch_limit(&r);assert(r.revision==revision);
        bridge_parameters(&b,r.mode,r.forward_ms,r.reverse_ms);
        bridge_request(&b,r.revision,r.motion,r.speed,r.ack,12);
        assert(!b.queued);assert(bridge_tick(&b,12,&out));
        assert(out.payload[0]==0 && out.payload[1]==0 && out.payload[8]==0);
        unsigned before=stops;assert(peer_receive(&p,&out,12,&reply));assert(stops>before);
        assert(bridge_receive(&b,&reply,12));assert(p.motion==0);
        uint8_t cmd[16]={3,1,50};cmd[4]=1;
        assert(!robot_command(&r,cmd,sizeof(cmd),13));
        robot_connection(&r,false);robot_connection(&r,true);robot_tick(&r,14);
        assert(r.limit_latched && r.error==ROBOT_LIMIT_STOP && r.motion==0);
        assert(!robot_command(&r,cmd,sizeof(cmd),15));
        uint8_t report[20];robot_status(&r,15,report);assert(report[7]==ROBOT_LIMIT_STOP);
        printf("PASS v%u limit preempts queued motion, peer immediate STOP, persistent inhibit and error 10\n",protocol);
    }
    return 0;
}
