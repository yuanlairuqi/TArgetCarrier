#include "pulse_wire.h"
#include <stdlib.h>
#include <string.h>
static void put16(uint8_t *p, uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p, uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static int compare(const void *a,const void *b){
    uint32_t x=((const pulse_edge_t *)a)->tick,y=((const pulse_edge_t *)b)->tick;
    return (x>y)-(x<y);
}
void pulse_finalize(pulse_snapshot_t *s){
    if(s->count>PULSE_MAX_EDGES){s->count=PULSE_MAX_EDGES;s->flags|=PULSE_OVERFLOW;}
    qsort(s->edges,s->count,sizeof(s->edges[0]),compare);
    unsigned seen=0,levels=s->initial;
    for(unsigned i=0;i<s->count;i++){
        pulse_edge_t *e=&s->edges[i];unsigned channel=e->tag&1U,mask=1U<<channel,value=(e->tag>>1)&1U;
        if(e->tick>=s->duration || (e->tag&~3U))s->flags|=PULSE_EDGE_LOSS;
        if(!(seen&mask)){
            // The first actual edge determines the level immediately before it.
            s->initial=(uint8_t)((s->initial&~mask)|((value^1U)<<channel));
            seen|=mask;
        }else if(((levels>>channel)&1U)==value)s->flags|=PULSE_EDGE_LOSS;
        levels=(levels&~mask)|(value<<channel);
    }
}
static void header(const pulse_snapshot_t *s,uint8_t *p){
    memset(p,0,20);p[0]=0x57;p[1]=1;put16(p+2,s->id);p[4]=s->flags;p[5]=s->initial;
    put32(p+6,s->hz);put32(p+10,s->duration);put16(p+14,s->count);
    p[16]=s->pul_gpio;p[17]=s->dir_gpio;p[18]=1;
}
static uint32_t crc_bytes(uint32_t crc,const uint8_t *p,size_t n){
    for(size_t i=0;i<n;i++){crc^=p[i];for(unsigned j=0;j<8;j++)crc=(crc>>1)^((crc&1U)?0xedb88320U:0U);}
    return crc;
}
static uint32_t checksum(const pulse_snapshot_t *s){
    uint8_t b[20];header(s,b);uint32_t crc=crc_bytes(0xffffffffU,b,20);
    for(unsigned i=0;i<s->count;i++){put32(b,s->edges[i].tick);b[4]=s->edges[i].tag;crc=crc_bytes(crc,b,5);}
    return crc^0xffffffffU;
}
size_t pulse_packet_count(const pulse_snapshot_t *s){return 2U+(s->count+2U)/3U;}
bool pulse_packet(const pulse_snapshot_t *s,size_t index,uint8_t out[20]){
    if(s->count>PULSE_MAX_EDGES||index>=pulse_packet_count(s))return false;
    if(index==0){header(s,out);return true;}
    memset(out,0,20);out[0]=0x57;put16(out+2,s->id);
    if(index==pulse_packet_count(s)-1){
        out[1]=3;put16(out+4,(uint16_t)((s->count+2U)/3U));put16(out+6,s->count);put32(out+8,checksum(s));
    }else{
        out[1]=2;out[4]=(uint8_t)(index-1);
        for(unsigned n=0;n<3;n++){
            size_t e=(index-1)*3+n;if(e>=s->count)break;
            put32(out+5+n*5,s->edges[e].tick);out[9+n*5]=s->edges[e].tag;
        }
    }
    return true;
}
