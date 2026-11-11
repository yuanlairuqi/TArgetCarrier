#include "now_protocol.h"
#include <string.h>
static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;
}
static void put32(uint8_t *p, uint32_t v) {
    for (int i=0;i<4;++i) p[i]=(uint8_t)(v>>(8*i));
}
uint32_t now_crc32(const uint8_t *p, size_t n) {
    uint32_t crc=0xffffffffu;
    for (size_t i=0;i<n;++i) {
        crc^=p[i];
        for (int b=0;b<8;++b) crc=(crc>>1)^((crc&1)?0xedb88320u:0);
    }
    return crc^0xffffffffu;
}
bool now_decode(const uint8_t *d, size_t len, now_request_t *q) {
    if (!d||!q||len!=NOW_FRAME_SIZE||d[0]!='W'||d[1]!='N'||d[2]!=1||
        (d[3]!=NOW_QUERY&&d[3]!=NOW_COMMAND)||get32(d+60)!=now_crc32(d,60)) return false;
    for (int i=40;i<60;++i) if(d[i]) return false;
    if(d[3]==NOW_QUERY) for(int i=8;i<40;++i) if(d[i]) return false;
    memset(q,0,sizeof(*q));q->kind=d[3];q->node=get32(d+4);q->boot=get32(d+8);
    q->epoch=get32(d+12);q->session=get32(d+16);q->seq=get32(d+20);
    memcpy(q->command,d+24,ROBOT_CMD_SIZE);
    return true;
}
static void revoke(now_control_t *n) {
    n->active=false;n->session=0;
    if (++n->epoch==0) ++n->epoch;
}
void now_control_init(now_control_t *n, uint32_t node, uint32_t boot) {
    memset(n,0,sizeof(*n));n->node=node;n->boot=boot?boot:1;n->epoch=1;
}
void now_ble_connection(now_control_t *n, robot_control_t *r, bool connected) {
    revoke(n);n->ble_connected=connected;robot_connection(r,connected);
}
void now_control_link_lost(now_control_t *n,robot_control_t *r) {
    revoke(n);
    if(!n->ble_connected){robot_connection(r,false);robot_stop(r,ROBOT_DISCONNECTED);}
}
void now_control_tick(now_control_t *n, robot_control_t *r, uint32_t now) {
    if(n->active&&((uint32_t)(now-n->last_fresh_ms)>=ROBOT_TIMEOUT_MS||r->error!=ROBOT_OK)) {
        uint8_t error=r->error?r->error:ROBOT_TIMEOUT;
        revoke(n);robot_connection(r,false);robot_stop(r,error);
    }
}
static bool command_valid(const uint8_t *d, uint32_t seq) {

    if(d[0]==4){
        uint32_t f=((uint32_t)d[8]|(uint32_t)d[9]<<8)*100U,r=((uint32_t)d[10]|(uint32_t)d[11]<<8)*100U;
        bool rates=(d[12]||d[13])&&(d[14]||d[15]);
        return d[1]<=3 && d[2]<=100 && d[3]<=1 && rates && get32(d+4)==seq &&
            ((d[1]==0||d[1]==3)?d[2]==0:d[2]>0) &&
            ((d[1]!=0&&d[1]!=3)||(!d[3]&&!f&&!r)) &&
            (!d[3]||(f>=100&&r>=100&&f<=3600000&&r<=3600000));
    }
    return d[0]==3&&d[1]<=ROBOT_BACKWARD&&d[2]<=100&&d[3]<=1&&
        ((d[1]==ROBOT_STOP)==(d[2]==0))&&get32(d+4)==seq&&
        (d[3]==0||(get32(d+8)>=100&&get32(d+8)<=3600000&&
                    get32(d+12)>=100&&get32(d+12)<=3600000));
}
uint8_t now_control_command(now_control_t *n, robot_control_t *r,
                          const now_request_t *q, uint32_t received, uint32_t now) {
    now_control_tick(n,r,now);
    if(q->node!=n->node||q->kind!=NOW_COMMAND) return NOW_BAD_COMMAND;
    if(n->ble_connected) return NOW_BUSY;
    if(q->boot!=n->boot||q->epoch!=n->epoch) return NOW_CHALLENGE;
    if(!q->session) return NOW_SESSION;
    if(n->active&&q->session!=n->session) return NOW_SESSION;
    if(!command_valid(q->command,q->seq)) {
        if(n->active) {robot_stop(r,ROBOT_BAD_COMMAND);now_control_tick(n,r,now);}
        return NOW_BAD_COMMAND;
    }
    if((uint32_t)(now-received)>NOW_QUEUE_MAX_AGE_MS) {
        if(q->command[1]==ROBOT_STOP) robot_stop(r,r->error);
        return NOW_STALE;
    }
    if(!n->active) {
        if(q->command[1]!=ROBOT_STOP) return NOW_NEED_STOP;
        n->active=true;n->session=q->session;robot_connection(r,true);
    }
    uint32_t delta=q->seq-r->ack;
    if(r->has_seq&&(delta==0||delta>=0x80000000u)) {
        if(q->command[1]==ROBOT_STOP) robot_stop(r,r->error);
        return NOW_REPLAY;
    }
    // Remote movement always requires the actual UART/STM32 path, even in a BLE demo build.
    if(q->command[1]!=ROBOT_STOP&&(!r->uart_configured||!robot_can_move(r))) {
        robot_stop(r,ROBOT_STM_OFFLINE);now_control_tick(n,r,now);return NOW_BAD_COMMAND;
    }
    if(!robot_command(r,q->command,ROBOT_CMD_SIZE,now)) {
        now_control_tick(n,r,now);return NOW_BAD_COMMAND;
    }
    n->last_fresh_ms=now;
    return NOW_OK;
}
void now_status_frame(const now_control_t *n, const robot_control_t *r,
                      const now_request_t *q, uint8_t result, uint32_t now,
                      uint8_t out[NOW_FRAME_SIZE]) {
    memset(out,0,NOW_FRAME_SIZE);out[0]='W';out[1]='N';out[2]=1;out[3]=NOW_STATUS;
    put32(out+4,n->node);put32(out+8,n->boot);put32(out+12,n->epoch);
    put32(out+16,q?q->session:n->session);put32(out+20,q?q->seq:r->ack);
    out[24]=result;out[25]=n->ble_connected?1:(n->active?2:0);out[26]=q?q->kind:0;
    robot_status(r,now,out+40);put32(out+60,now_crc32(out,60));
}
