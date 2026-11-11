#include "now_session.h"
#include "ble_guard.h"
uint8_t tn_accept(tn_session_t *s,const now_request_t *q,uint32_t boot,uint32_t epoch,uint32_t at,uint32_t now){
 if(q->node!=2||q->kind!=5||!tb_frame_valid(q->command,17))return NOW_BAD_COMMAND;
 bool stop=q->command[2]==PROTO_MSG_CTRL&&(!(q->command[4]&1)||(q->command[4]&12));
 if(q->boot!=boot||q->epoch!=epoch)return NOW_CHALLENGE;
 if(!q->session||(s->active&&q->session!=s->session))return NOW_SESSION;
 if(now-at>100U)return NOW_STALE;
 if(!s->active&&!stop)return NOW_NEED_STOP;
 if(s->active&&(!(uint32_t)(q->seq-s->seq)||(uint32_t)(q->seq-s->seq)>=0x80000000U))return NOW_REPLAY;
 s->active=true;s->session=q->session;s->seq=q->seq;s->last=now;return NOW_OK;
}
