#include "ble_guard.h"
#include <string.h>
static void safe(tb_guard_t *g){
    if(g->active)g->blocked_token=g->token;
    g->active=false;g->token=0;g->disarm_seen=false;g->failsafe=true;
}
void tb_guard_init(tb_guard_t *g){memset(g,0,sizeof(*g));g->failsafe=true;lc_seq_gate_init(&g->seq);}
void tb_guard_connection(tb_guard_t *g,bool connected){safe(g);g->connected=connected;g->have_pc=false;lc_seq_gate_init(&g->seq);}
bool tb_frame_valid(const uint8_t *f,size_t size){
    if(!f||size!=17)return false;
    if(f[2]==PROTO_MSG_CTRL){ctrl_payload_t p;uint8_t seq;
        if(!proto_decode_ctrl(f,17,&p,&seq))return false;
        const int limit=(p.flags & CTRL_FLAG_VELOCITY)?(int)(MANUAL_YAW_MAX_DPS/PROTO_RATE_SCALE):1800;
        return p.mode<=MOTOR_MODE_PSI&&p.yaw_q>=-18000&&p.yaw_q<=18000&&
            p.pitch_q>=-3000&&p.pitch_q<=6000&&p.yawrate_q>=-limit&&p.yawrate_q<=limit&&p.pitchrate_q>=-1800&&p.pitchrate_q<=1800;}
    if(f[2]==PROTO_MSG_GAINS){float kp,kd;return proto_decode_gains(f,17,&kp,&kd)&&kp>=0&&kp<=200&&kd>=0&&kd<=3;}
    if(f[2]==PROTO_MSG_PARAM){uint8_t op;return proto_decode_param(f,17,&op,NULL,NULL)&&op>=1&&op<=4;}
    return false;
}
bool tb_guard_tick(tb_guard_t *g,uint32_t now,bool stm_fresh){
    bool failed=!g->connected || (g->have_pc&&(uint32_t)(now-g->last_pc)>=600U) || (g->active&&!stm_fresh);
    if(failed&&!g->failsafe){safe(g);return true;}
    return false;
}
bool tb_guard_accept(tb_guard_t *g,const uint8_t f[17],uint32_t now,bool stm_fresh){
    if(!g->connected||!tb_frame_valid(f,17)){++g->rejected;return false;}
    (void)tb_guard_tick(g,now,stm_fresh);
    if(f[2]!=PROTO_MSG_CTRL){++g->accepted;return true;}
    ctrl_payload_t p;uint8_t seq;
    if(!proto_decode_ctrl(f,17,&p,&seq)||!lc_seq_accept(&g->seq,seq)){++g->rejected;return false;}
    bool enabled=(p.flags&CTRL_FLAG_ENABLE)!=0;
    if(!enabled||(p.flags&(CTRL_FLAG_ESTOP|CTRL_FLAG_FAILSAFE))){
        if(g->active)g->blocked_token=g->token;
        g->active=false;g->token=0;g->disarm_seen=true;g->failsafe=false;
    }else{
        uint8_t token=proto_flags_token(p.flags);
        if(!stm_fresh||!g->disarm_seen||g->failsafe||!(p.flags&CTRL_FLAG_ARM)||!token||
           (!g->active&&token==g->blocked_token)|| (g->active&&token!=g->token)){
            ++g->rejected;return false;
        }
        g->active=true;g->token=token;
    }
    g->have_pc=true;g->last_pc=now;++g->accepted;return true;
}
