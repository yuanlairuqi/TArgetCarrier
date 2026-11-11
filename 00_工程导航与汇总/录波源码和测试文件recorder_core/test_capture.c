#include "capture.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){uint8_t b[CAPTURE_WIRE_SIZE];cap_record a={.kind=CAP_SAMPLE,.type=CAP_F32,.channel=7,.unit=4,.source=2,.boot=37,.seq=0xffffffffu,.sample_us=0x123456789abcdef0ULL,.value=0x3f800000u},z;assert(cap_encode(&a,b));assert(cap_decode(b,&z));assert(z.channel==7&&z.source==2&&z.boot==37&&z.seq==0xffffffffu&&z.sample_us==a.sample_us&&z.value==a.value);for(unsigned i=0;i<CAPTURE_WIRE_SIZE;i++){b[i]^=1;assert(!cap_decode(b,&z));b[i]^=1;}cap_queue q={0};for(unsigned i=0;i<CAPTURE_QUEUE_SIZE;i++){a.seq=i;assert(cap_push(&q,&a));}assert(!cap_push(&q,&a)&&q.dropped==1&&q.high_water==CAPTURE_QUEUE_SIZE);for(unsigned i=0;i<CAPTURE_QUEUE_SIZE;i++){assert(cap_pop(&q,&z));assert(z.seq==i);}assert(!cap_pop(&q,&z));puts("PASS record codec/CRC, timestamps, queue overflow, order");}
