#include "source.h"
#include <assert.h>
#include <stdio.h>
static frag_join join;static cap_record result;static unsigned complete;static bool ready=true;
static bool send(const uint8_t*f,void*ctx){(void)ctx;if(!ready)return false;const uint8_t*p;size_t n;int rc=frag_accept(&join,f,complete*100,&p,&n);if(rc==1){assert(n==40&&cap_decode(p,&result));complete++;}return true;}
int main(void){cap_source s;cap_source_init(&s,2,3);
 assert(cap_source_feed(&s,CAP_SAMPLE,1,CAP_U32,1,100000,42));
 for(unsigned t=100;t<300;t+=40)cap_source_poll(&s,true,(uint64_t)t*1000,send,0);
 assert(complete==1&&result.value==42&&result.sample_us==100000);
 for(unsigned i=0;i<40;i++)cap_source_feed(&s,CAP_EVENT,100,CAP_U32,0,300000,i);
 assert(s.queue.dropped==8);cap_source_poll(&s,false,300000,send,0);assert(!s.pending&&!s.queue.count&&s.lost==32);
 assert(cap_source_feed(&s,CAP_SAMPLE,1,CAP_U32,1,400000,9));ready=false;
 cap_source_poll(&s,true,400000,send,0);cap_source_poll(&s,true,850000,send,0);assert(s.lost>=33);
 ready=true;cap_source_poll(&s,false,900000,send,0);assert(!s.pending);puts("source backpressure/fragment/overflow/loss/reconnect PASS");return 0;}
