#include "source.h"
#include <string.h>
void cap_source_init(cap_source*s,uint32_t node,uint32_t boot){memset(s,0,sizeof(*s));s->source=node;s->boot=boot;}
bool cap_source_feed(cap_source*s,uint8_t kind,uint16_t channel,uint8_t type,uint16_t unit,uint64_t us,uint32_t value){
 cap_record r={.kind=kind,.type=type,.channel=channel,.unit=unit,.source=s->source,.boot=s->boot,.seq=++s->seq,.sample_us=us,.value=value};
 uint8_t test[40];if(!cap_encode(&r,test)){return false;}return cap_push(&s->queue,&r);
}
void cap_source_poll(cap_source*s,bool active,uint64_t now_us,cap_send_fn send,void*ctx){
 uint32_t now=(uint32_t)(now_us/1000);
 if(!active){s->lost+=s->queue.count+(s->pending?1:0);s->queue.count=0;s->queue.head=0;s->pending=false;return;}
 if(s->pending&&(uint32_t)(now-s->started)>400){s->pending=false;s->lost++;}
 /* Report source-side queue/expiry losses as an explicit state channel. */
 static const uint16_t loss_channel=65535;
 if(!s->pending && (s->queue.dropped+s->lost)!=s->reported){
  s->reported=s->queue.dropped+s->lost;
  (void)cap_source_feed(s,CAP_STATE,loss_channel,CAP_U32,0,now_us,s->reported);
 }
 if(!s->pending){cap_record r;if(!cap_pop(&s->queue,&r))return;
  /* Stale optional samples are discarded, never replayed after reconnect. */
  if(now_us>=r.sample_us&&now_us-r.sample_us>1000000){s->lost++;return;}
  if(!cap_encode(&r,s->wire)){return;}s->index=0;s->total=4;s->pending=true;s->started=now;if(!++s->batch)++s->batch;
 }
 if((uint32_t)(now-s->last_send)<40)return;
 uint8_t f[20];if(frag_encode(s->wire,40,s->batch,s->index,f)&&send(f,ctx)){
  s->last_send=now;if(++s->index==s->total)s->pending=false;
 }
}
