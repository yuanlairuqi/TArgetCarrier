#include "recorder.h"
#include <assert.h>
#include <stdio.h>
static int fail;static unsigned writes;static bool write_cb(void*c,const uint8_t*b,size_t n){(void)c;cap_record x;assert(n==40&&cap_decode(b,&x));if(fail)return false;writes++;return true;}
int main(void){recorder r;cap_record x={.kind=CAP_EVENT,.type=CAP_U32,.source=1,.boot=1};rec_init(&r,write_cb,0);assert(rec_start(&r));for(int i=0;i<32;i++)assert(rec_submit(&r,&x));assert(!rec_submit(&r,&x)&&r.state==REC_OVERFLOW);assert(rec_drain(&r,32)&&writes==32&&r.queue.dropped==1);assert(rec_stop(&r));assert(rec_start(&r));assert(rec_submit(&r,&x));fail=1;assert(!rec_drain(&r,1)&&r.state==REC_IO_ERROR&&r.queue.count==1);assert(!rec_stop(&r));puts("PASS bounded recorder, overflow, write failure and no silent retirement");}
