#include "capture.h"
#include <string.h>
static void put16(uint8_t *p,uint16_t n){p[0]=(uint8_t)n;p[1]=(uint8_t)(n>>8);}
static void put32(uint8_t *p,uint32_t n){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(n>>(8*i));}
static uint16_t get16(const uint8_t*p){return (uint16_t)(p[0]|((uint16_t)p[1]<<8));}
static uint32_t get32(const uint8_t*p){uint32_t n=0;for(unsigned i=0;i<4;i++)n|=(uint32_t)p[i]<<(8*i);return n;}
static uint32_t crc32(const uint8_t*p,size_t n){uint32_t c=~0u;while(n--){c^=*p++;for(unsigned i=0;i<8;i++)c=(c>>1)^((0u-(c&1u))&0xedb88320u);}return ~c;}
static bool valid(const cap_record *r){return r && r->kind>=CAP_SAMPLE && r->kind<=CAP_EVENT && r->type>=CAP_U32 && r->type<=CAP_F32 && r->source && r->boot;}
bool cap_encode(const cap_record*r,uint8_t*out){if(!valid(r)||!out)return false;memset(out,0,CAPTURE_WIRE_SIZE);out[0]='C';out[1]='R';out[2]=1;out[3]=r->kind;out[4]=r->type;put16(out+6,r->channel);put16(out+8,r->unit);put32(out+12,r->source);put32(out+16,r->boot);put32(out+20,r->seq);put32(out+24,(uint32_t)r->sample_us);put32(out+28,(uint32_t)(r->sample_us>>32));put32(out+32,r->value);put32(out+36,crc32(out,36));return true;}
bool cap_decode(const uint8_t*in,cap_record*r){if(!in||!r||in[0]!='C'||in[1]!='R'||in[2]!=1||in[5]||in[10]||in[11]||get32(in+36)!=crc32(in,36))return false;cap_record t={0};t.kind=in[3];t.type=in[4];t.channel=get16(in+6);t.unit=get16(in+8);t.source=get32(in+12);t.boot=get32(in+16);t.seq=get32(in+20);t.sample_us=(uint64_t)get32(in+24)|((uint64_t)get32(in+28)<<32);t.value=get32(in+32);if(!valid(&t))return false;*r=t;return true;}
bool cap_push(cap_queue*q,const cap_record*r){if(!q||!valid(r))return false;if(q->count==CAPTURE_QUEUE_SIZE){q->dropped++;return false;}q->items[(q->head+q->count)%CAPTURE_QUEUE_SIZE]=*r;q->count++;if(q->count>q->high_water)q->high_water=q->count;return true;}
bool cap_pop(cap_queue*q,cap_record*r){if(!q||!r||!q->count)return false;*r=q->items[q->head];q->head=(q->head+1)%CAPTURE_QUEUE_SIZE;q->count--;return true;}
