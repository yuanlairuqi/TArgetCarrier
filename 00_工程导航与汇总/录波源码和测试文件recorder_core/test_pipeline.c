#include "capture.h"
#include "batch.h"
#include "fragment.h"
#include "recorder.h"
#include <assert.h>
#include <stdio.h>
static unsigned written;static bool write_cb(void*c,const uint8_t*b,size_t n){(void)c;cap_record v;assert(n==40&&cap_decode(b,&v));assert(v.seq==written&&v.sample_us==1000+written*40000&&v.value==written+100);written++;return true;}
int main(void){cap_batch in={.source=1,.boot=123,.first_us=1000,.period_us=40000,.first_seq=0,.channel=2,.unit=1,.type=1,.count=8},batch;for(unsigned i=0;i<8;i++)in.values[i]=100+i;uint8_t data[72],frag[20];size_t total=cap_batch_encode(&in,data,72),n=0;assert(total==72);frag_join join={0};const uint8_t*out=0;for(unsigned i=0;i<6;i++){assert(frag_encode(data,total,1,i,frag));int v=frag_accept(&join,frag,i*20,&out,&n);assert(v==(i==5));}assert(cap_batch_decode(out,n,&batch));recorder r;rec_init(&r,write_cb,0);assert(rec_start(&r));for(unsigned i=0;i<batch.count;i++){cap_record record={.kind=CAP_SAMPLE,.type=batch.type,.channel=batch.channel,.unit=batch.unit,.source=batch.source,.boot=batch.boot,.seq=batch.first_seq+i,.sample_us=batch.first_us+(uint64_t)i*batch.period_us,.value=batch.values[i]};assert(rec_submit(&r,&record));}assert(rec_drain(&r,8));assert(written==8&&rec_stop(&r));puts("PASS in-memory batch->fragments->records->storage reconstruction");}
