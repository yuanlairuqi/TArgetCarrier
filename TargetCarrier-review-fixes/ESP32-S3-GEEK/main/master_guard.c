#include "master_guard.h"
void master_guard_loss(master_guard_t *g){
    if(g->active)g->blocked=g->session;
    g->active=false;g->unlocked=false;g->queried=false;++g->losses;
}
void master_guard_query(master_guard_t *g,uint32_t now){g->queried=true;g->last_pc=now;}
bool master_guard_status(master_guard_t *g,uint32_t boot,uint32_t epoch,uint32_t now){
    if(!boot||!epoch)return false;
    if(g->challenge&&(boot!=g->boot||epoch!=g->epoch)){
        for(unsigned i=0;i<4;i++)if(g->retired_boot[i]==boot&&g->retired_epoch[i]==epoch)return false;
        if(boot==g->boot && (uint32_t)(epoch-g->epoch)>=0x80000000U)return false;
        unsigned slot=g->retired_at++%4;g->retired_boot[slot]=g->boot;g->retired_epoch[slot]=g->epoch;
        master_guard_loss(g);
    }
    g->boot=boot;g->epoch=epoch;g->challenge=true;g->last_radio=now;return true;
}
bool master_guard_command(master_guard_t *g,const now_request_t *q,uint32_t now){
    if(!g->queried||!g->challenge||!q->session||q->session==g->blocked||
        q->boot!=g->boot||q->epoch!=g->epoch||now-g->last_radio>=MASTER_TRANSPORT_MS)return false;
    bool stop=q->command[1]==ROBOT_STOP;
    if(!g->active){
        if(!stop)return false;
        g->session=q->session;g->active=true;g->unlocked=false;g->last_seq=q->seq;
    }else{
        if(g->session!=q->session||(!stop&&!g->unlocked))return false;
        uint32_t delta=q->seq-g->last_seq;
        if(!stop&&(!delta||delta>=0x80000000U))return false;
        if(delta&&delta<0x80000000U)g->last_seq=q->seq;
    }
    g->last_pc=now;return true;
}
bool master_guard_expired(const master_guard_t *g,bool usb,uint32_t now){
    return g->queried&&(!usb||now-g->last_pc>=MASTER_TRANSPORT_MS||(g->active&&now-g->last_radio>=MASTER_TRANSPORT_MS));
}
