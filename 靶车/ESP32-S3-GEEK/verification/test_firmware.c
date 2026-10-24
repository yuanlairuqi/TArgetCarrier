#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "now_protocol.h"
#include "usb_wire.h"
#include "pulse_wire.h"
static unsigned groups;
static now_request_t req(now_control_t *n,uint32_t seq,uint8_t motion){
    now_request_t q={.kind=NOW_COMMAND,.node=n->node,.boot=n->boot,.epoch=n->epoch,.session=42,.seq=seq};
    q.command[0]=3;q.command[1]=motion;q.command[2]=motion?40:0;wire_put32(q.command+4,seq);return q;
}
static void ready(robot_control_t *r){robot_init(r);r->uart_configured=true;r->stm_online=true;r->stm_flags=1|8;}
int main(void){
    now_control_t n;robot_control_t r;ready(&r);now_control_init(&n,1,7);
    now_request_t q=req(&n,1,1);assert(now_control_command(&n,&r,&q,0,0)==NOW_NEED_STOP);++groups;
    q=req(&n,1,0);assert(now_control_command(&n,&r,&q,0,0)==NOW_OK);assert(n.active&&r.motion==0);++groups;
    q=req(&n,2,1);assert(now_control_command(&n,&r,&q,100,100)==NOW_OK);uint32_t rev=r.revision;
    assert(now_control_command(&n,&r,&q,500,500)==NOW_REPLAY);assert(r.revision==rev&&n.last_fresh_ms==100);++groups;
    now_control_tick(&n,&r,900);assert(!n.active&&r.motion==0&&r.error==ROBOT_TIMEOUT);++groups;
    q=req(&n,3,1);assert(now_control_command(&n,&r,&q,901,901)==NOW_NEED_STOP);++groups;
    q=req(&n,4,0);assert(now_control_command(&n,&r,&q,1000,1000)==NOW_OK);
    q=req(&n,5,1);assert(now_control_command(&n,&r,&q,1001,1102)==NOW_STALE);assert(r.motion==0);++groups;
    q=req(&n,5,1);q.session=99;assert(now_control_command(&n,&r,&q,1103,1103)==NOW_SESSION);++groups;
    q=req(&n,5,1);q.node=2;assert(now_control_command(&n,&r,&q,1103,1103)==NOW_BAD_COMMAND);++groups;
    q=req(&n,5,1);q.boot=8;assert(now_control_command(&n,&r,&q,1103,1103)==NOW_CHALLENGE);++groups;
    now_ble_connection(&n,&r,true);q=req(&n,5,1);assert(now_control_command(&n,&r,&q,1103,1103)==NOW_BUSY);assert(r.motion==0);++groups;
    now_ble_connection(&n,&r,false);ready(&r);q=req(&n,1,0);assert(now_control_command(&n,&r,&q,1200,1200)==NOW_OK);
    q=req(&n,2,1);assert(now_control_command(&n,&r,&q,1300,1300)==NOW_OK);
    q=req(&n,3,0);assert(now_control_command(&n,&r,&q,1301,1301)==NOW_OK);assert(r.motion==0);++groups;
    r.mode=1;r.forward_ms=1000;r.reverse_ms=1000;
    q=req(&n,3,0);assert(now_control_command(&n,&r,&q,1302,1302)==NOW_REPLAY);assert(r.motion==0&&r.mode==0&&r.forward_ms==0&&r.reverse_ms==0);++groups;
    uint8_t body[64]={ 'W','N',1,1 };wire_put32(body+4,1);wire_put32(body+60,now_crc32(body,60));
    assert(now_decode(body,64,&q));body[2]=2;assert(!now_decode(body,64,&q));body[2]=1;
    assert(!now_decode(body,63,&q));body[30]=1;assert(!now_decode(body,64,&q));body[30]=0;++groups;
    uint8_t usb[72],decoded[64],kind;usb_parser_t parser={0};usb_pack(1,body,usb);
    for(unsigned i=0;i<71;i++)assert(!usb_feed(&parser,usb[i],decoded,&kind));
    assert(usb_feed(&parser,usb[71],decoded,&kind)&&kind==1&&!memcmp(decoded,body,64));++groups;
    usb[71]^=1;for(unsigned i=0;i<72;i++)assert(!usb_feed(&parser,usb[i],decoded,&kind));
    usb[71]^=1;for(unsigned i=0;i<72;i++){bool got=usb_feed(&parser,usb[i],decoded,&kind);assert(got==(i==71));}++groups;
    for(unsigned count=0;count<=512;count++){
        pulse_snapshot_t s={.id=7,.count=count,.hz=1000,.duration=1000,.flags=1,.pul_gpio=6,.dir_gpio=7};
        for(unsigned i=0;i<count;i++){s.edges[i].tick=i+1;s.edges[i].tag=(i&1)?0:2;}
        pulse_finalize(&s);size_t total=pulse_packet_count(&s);assert(total==2+(count+2)/3);
        uint8_t p[20];for(size_t i=0;i<total;i++)assert(pulse_packet(&s,i,p));assert(p[1]==3);
        assert(!pulse_packet(&s,total,p));
    }++groups;
    uint8_t command_frame[64]={'W','N',1,2},status_frame[64]={'W','N',1,3};
    wire_put32(command_frame+4,1);wire_put32(command_frame+8,7);wire_put32(command_frame+12,1);wire_put32(command_frame+16,42);wire_put32(command_frame+20,9);
    memcpy(status_frame+4,command_frame+4,20);status_frame[24]=0;status_frame[25]=2;status_frame[40]=3;status_frame[42]=185;
    wire_put32(status_frame+48,9);wire_put32(status_frame+52,9);
    assert(master_output_confirmed(status_frame,command_frame));
    status_frame[25]=1;assert(!master_output_confirmed(status_frame,command_frame));status_frame[25]=2;
    status_frame[16]^=1;assert(!master_output_confirmed(status_frame,command_frame));status_frame[16]^=1;
    status_frame[42]&=~32;assert(!master_output_confirmed(status_frame,command_frame));status_frame[42]|=32;
    status_frame[52]=8;assert(!master_output_confirmed(status_frame,command_frame));++groups;
    printf("PASS: %u firmware host groups (including all 513 waveform edge counts)\n",groups);
}
