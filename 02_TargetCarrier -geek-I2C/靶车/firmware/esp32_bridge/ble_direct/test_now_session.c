#include "now_session.h"
#include "ble_guard.h"
#include <assert.h>
#include <stdio.h>
static now_request_t req(uint32_t seq,uint16_t flags){now_request_t q={.kind=5,.node=2,.boot=7,.epoch=1,.session=11,.seq=seq};ctrl_target_t t={0};proto_encode_ctrl(q.command,&t,flags,MOTOR_MODE_MIT,(uint8_t)seq);return q;}
int main(void){
 tn_session_t s={0};now_request_t q=req(1,CTRL_FLAG_ENABLE|CTRL_FLAG_ARM);
 assert(tn_accept(&s,&q,7,1,0,0)==NOW_NEED_STOP&&!s.active);
 q=req(2,CTRL_FLAG_ESTOP);assert(tn_accept(&s,&q,7,1,0,0)==NOW_OK&&s.active);
 assert(tn_accept(&s,&q,7,1,1,1)==NOW_REPLAY);q.seq=3;q.session=22;assert(tn_accept(&s,&q,7,1,1,1)==NOW_SESSION);
 q.session=11;q.epoch=0;assert(tn_accept(&s,&q,7,1,1,1)==NOW_CHALLENGE);q.epoch=1;assert(tn_accept(&s,&q,7,1,0,101)==NOW_STALE);
 q.node=1;assert(tn_accept(&s,&q,7,1,101,101)==NOW_BAD_COMMAND);
 q=req(3,CTRL_FLAG_ESTOP);s.active=false;s.session=0;assert(tn_accept(&s,&q,7,2,1,1)==NOW_CHALLENGE);
 q.epoch=2;assert(tn_accept(&s,&q,7,2,1,1)==NOW_OK);
 tb_guard_t g;tb_guard_init(&g);tb_guard_connection(&g,true);assert(tb_guard_accept(&g,q.command,1,false));
 q=req(4,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,8));q.epoch=2;
 assert(tn_accept(&s,&q,7,2,2,2)==NOW_OK);assert(!tb_guard_accept(&g,q.command,2,false));
 q=req(5,proto_flags_set_token(CTRL_FLAG_ENABLE|CTRL_FLAG_ARM,8));assert(tb_guard_accept(&g,q.command,3,true));
 assert(tb_guard_tick(&g,4,false)&&g.failsafe&&!g.active);
 puts("PASS target session STOP acquisition / node / duplicate / stale / old epoch / owner / STM freshness");return 0;
}
