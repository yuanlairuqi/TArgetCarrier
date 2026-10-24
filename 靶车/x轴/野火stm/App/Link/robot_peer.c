#include "robot_peer.h"
#include <string.h>
static bool ready(robot_peer_t *p) {
    return !p->hardware||(p->hooks.ready&&p->hooks.apply&&p->hooks.stop&&p->hooks.ready(p->hooks.ctx));
}
void peer_force_stop(robot_peer_t *p,uint8_t error) {
    if(p->hooks.stop)p->hooks.stop(p->hooks.ctx);
    p->motion=0;p->speed=0;p->applied=false;p->error=error;
}
void peer_init(robot_peer_t *p,bool hardware,const peer_hooks_t *hooks) {
    memset(p,0,sizeof(*p));p->hardware=hardware;if(hooks)p->hooks=*hooks;
    peer_force_stop(p,PEER_OK);
}

void peer_tick(robot_peer_t *p,uint32_t now) {
    if(p->extended){
        uint8_t dir=0,phase=0;uint16_t hz=0,target=0;bool active=false;
        if(p->hooks.profile_status)p->hooks.profile_status(p->hooks.ctx,&dir,&phase,&hz,&target,&active);
        if((active || p->motion!=0) && (!ready(p)||(uint32_t)(now-p->lease_at)>=p->lease_ms))
            peer_force_stop(p,ready(p)?PEER_TIMEOUT:PEER_NOT_READY);
        return;
    }
    if(p->motion!=0&&(!ready(p)||(uint32_t)(now-p->lease_at)>=p->lease_ms))
        peer_force_stop(p,ready(p)?PEER_TIMEOUT:PEER_NOT_READY);
    if(p->motion!=0&&p->mode==1&&
       (uint32_t)(now-p->phase_at)>=(p->motion==1?p->forward_ms:p->reverse_ms)) {
        uint8_t next=p->motion==1?2:1;
        // The hardware hook must implement driver-specific ramp/DIR setup/dead time.
        if(p->hooks.stop)p->hooks.stop(p->hooks.ctx);
        if(p->hardware&&!p->hooks.apply(p->hooks.ctx,next,p->speed))peer_force_stop(p,PEER_DRIVER_ERROR);
        else {p->motion=next;p->phase_at=now;p->applied=p->hardware;}
    }
}
void peer_status(robot_peer_t *p,wire_frame_t *out) {
    memset(out,0,sizeof(*out));out->type=WIRE_STATUS;out->session=p->session;out->token=p->token;
    out->payload[0]=p->motion;out->payload[1]=p->speed;
    bool output=p->applied&&(p->motion==0||(p->hooks.active&&p->hooks.active(p->hooks.ctx)));
    out->payload[2]=(ready(p)?PEER_READY:0)|(!p->hardware?PEER_DEMO:0)|(output?PEER_APPLIED:0);
    if(p->hooks.local_busy&&p->hooks.local_busy(p->hooks.ctx))out->payload[2]|=PEER_LOCAL_BUSY;
    out->payload[3]=p->error;uint16_t mv=0xffff;
    if(p->hooks.battery&&p->hooks.battery(p->hooks.ctx,&mv)&&mv!=0xffff)out->payload[2]|=PEER_BATTERY;
    else mv=0xffff;
    out->payload[4]=(uint8_t)mv;out->payload[5]=(uint8_t)(mv>>8);wire_put32(out->payload+6,p->phone_seq);

    out->payload[10]=p->mode;out->payload[11]=p->motion;
    if(p->extended){
        uint8_t dir=0,phase=0;uint16_t hz=0,target=0;bool active=false;
        if(p->hooks.profile_status)p->hooks.profile_status(p->hooks.ctx,&dir,&phase,&hz,&target,&active);
        out->payload[0]=dir;out->payload[2]=(uint8_t)((ready(p)?PEER_READY:0)|PEER_PROFILE|(p->applied?PEER_APPLIED:0));
        out->payload[4]=(uint8_t)hz;out->payload[5]=(uint8_t)(hz>>8);
        out->payload[11]=phase;out->payload[12]=p->motion;
        out->payload[13]=(uint8_t)target;out->payload[14]=(uint8_t)(target>>8);
    }
}
bool peer_receive(robot_peer_t *p,const wire_frame_t *in,uint32_t now,wire_frame_t *out) {
    peer_tick(p,now);
    if(in->type==WIRE_PING) {
        // Ping reports health; it never extends a motion lease.
        if(!p->has_session||in->session!=p->session)return false;
        peer_status(p,out);return true;
    }

    if(in->type==WIRE_PROFILE){
        uint8_t op=in->payload[0],speed=in->payload[1],mode=in->payload[8];
        uint16_t ttl=(uint16_t)in->payload[2]|(uint16_t)in->payload[3]<<8;
        uint16_t a=(uint16_t)in->payload[14]|(uint16_t)in->payload[15]<<8;
        uint16_t d=(uint16_t)in->payload[16]|(uint16_t)in->payload[17]<<8;
        uint32_t f=((uint32_t)in->payload[10]|(uint32_t)in->payload[11]<<8)*100U;
        uint32_t r=((uint32_t)in->payload[12]|(uint32_t)in->payload[13]<<8)*100U;
        if(op>3 || speed>100 || ((op==0||op==3)?speed!=0:speed==0) || mode>1 || !a || !d ||
           ttl<100 || ttl>1000 || !in->session || in->payload[9]!=4 ||
           (mode&&(f<100||r<100||f>3600000||r>3600000)) ||
           ((op==0||op==3)&&(mode||f||r)) || !p->hooks.profile || !p->hooks.profile_status){
            peer_force_stop(p,PEER_BAD_COMMAND);if(p->has_session){peer_status(p,out);return true;}return false;
        }
        if(!p->has_session || in->session!=p->session){
            if(op!=0)return false;
            peer_force_stop(p,PEER_OK);p->has_session=true;p->session=in->session;p->has_token=false;
        }
        if(p->has_token&&!wire_newer(in->token,p->token)){
            if(op==0)peer_force_stop(p,p->error);
            peer_status(p,out);return true;
        }
        p->extended=true;p->has_token=true;p->token=in->token;p->phone_seq=wire_u32(in->payload+4);
        p->lease_at=now;p->lease_ms=ttl;p->mode=mode;p->speed=speed;p->motion=op;p->error=PEER_OK;
        p->applied=p->hooks.profile(p->hooks.ctx,op,speed,mode,f,r,a,d);
        if(!p->applied)peer_force_stop(p,PEER_DRIVER_ERROR);
        peer_status(p,out);return true;
    }
    if(in->type!=WIRE_COMMAND)return false;
    uint8_t op=in->payload[0],speed=in->payload[1],mode=in->payload[8];
    uint32_t forward_ms=wire_u32(in->payload+10),reverse_ms=wire_u32(in->payload+14);
    uint16_t ttl=(uint16_t)in->payload[2]|(uint16_t)in->payload[3]<<8;
    if(op>2||speed>100||(op==0&&speed!=0)||(op!=0&&speed==0)||
       ttl<100||ttl>1000||mode>1||in->payload[9]!=0||in->session==0||
       (mode==1&&(forward_ms<100||forward_ms>3600000||reverse_ms<100||reverse_ms>3600000))) {
        peer_force_stop(p,PEER_BAD_COMMAND);if(p->has_session){peer_status(p,out);return true;}return false;
    }
    if(!p->has_session||in->session!=p->session) {
        // A new ESP32 boot/session must first deliver STOP, never movement.
        if(op!=0)return false;
        peer_force_stop(p,PEER_OK);p->has_session=true;p->session=in->session;p->has_token=false;
    }
    if(p->has_token&&!wire_newer(in->token,p->token)) {
        if(op==0) {
            peer_force_stop(p,p->error);
            p->applied=p->hardware&&p->hooks.stopped&&p->hooks.stopped(p->hooks.ctx);
        }
        peer_status(p,out);return true;
    }
    p->extended=false; /* Only a validated, fresh legacy command may change the protocol mode. */
    bool keep_phase=p->motion!=0&&p->requested_motion==op&&p->mode==mode&&
        p->forward_ms==forward_ms&&p->reverse_ms==reverse_ms;
    bool same=keep_phase&&p->speed==speed;
    p->has_token=true;p->token=in->token;p->phone_seq=wire_u32(in->payload+4);
    p->mode=mode;p->forward_ms=forward_ms;p->reverse_ms=reverse_ms;p->requested_motion=op;
    p->lease_at=now;p->lease_ms=ttl;p->error=PEER_OK;p->applied=false;
    if(op==0) {
        peer_force_stop(p,PEER_OK);
        // STOP is accepted even by a controller with no continuous-motion capability.
        if(p->hardware&&p->hooks.stopped&&p->hooks.stopped(p->hooks.ctx))p->applied=true;
        peer_status(p,out);return true;
    }
    if(!ready(p))peer_force_stop(p,PEER_NOT_READY);
    else if(same) {p->applied=p->hardware;} // A heartbeat renews lease, never restarts the cycle.
    else if(keep_phase) {
        // Speed changes follow the current phase, including the reverse half of a cycle.
        if(p->hardware&&!p->hooks.apply(p->hooks.ctx,p->motion,speed))peer_force_stop(p,PEER_DRIVER_ERROR);
        else {p->speed=speed;p->applied=p->hardware;}
    }
    else {
        // apply() owns safe output changes/ramping and direction setup; do not reset PWM just for speed.
        if(p->hardware&&!p->hooks.apply(p->hooks.ctx,op,speed))peer_force_stop(p,PEER_DRIVER_ERROR);
        else {p->motion=op;p->speed=speed;p->phase_at=now;p->applied=p->hardware;}
    }
    peer_status(p,out);return true;
}
