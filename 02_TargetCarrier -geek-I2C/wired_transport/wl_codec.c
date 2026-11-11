#include "wl_codec.h"
#include <string.h>
static bool newer(uint32_t a,uint32_t b){return (int32_t)(a-b)>0;}
void wl_codec_init(wl_codec*c,bool m,uint8_t node,uint32_t session){memset(c,0,sizeof(*c));c->master=m;c->node=node;c->session=m?(session?session:1):0;c->bound=m;}
bool wl_encode(wl_codec*c,const uint8_t*d,unsigned n,bool safe,uint32_t now,uint8_t*out){if(!c->bound||!d||!n||n>WL_MAX)return false;if(++c->tx_seq==0)++c->tx_seq;tc_frame f={.type=c->master?TC_CONTROL:TC_STATUS,.session=c->session,.seq=c->tx_seq,.time=now,.count=1,.total=(uint16_t)(n+8),.len=(uint16_t)(n+8)};f.data[0]=0x4c;f.data[1]=c->node;f.data[2]=(uint8_t)n;f.data[3]=safe?1:0;tc_put32(f.data+4,c->master?0:c->echo);memcpy(f.data+8,d,n);return tc_encode(&f,out);}
bool wl_decode(wl_codec*c,const uint8_t*b,uint8_t*out,unsigned*n){tc_frame f;if(!tc_decode(b,&f)||f.type!=(c->master?TC_STATUS:TC_CONTROL)||f.count!=1||f.index||f.len<9||f.data[0]!=0x4c||f.data[1]!=c->node||f.data[2]>WL_MAX||f.len!=8+f.data[2]||f.data[3]>1){c->rejected++;return false;}
 uint32_t echo=tc_get32(f.data+4);
 if(c->master){if(f.session!=c->session||!echo||newer(echo,c->tx_seq)){c->rejected++;return false;}if(c->seen&&(!newer(echo,c->last_echo)&&(echo!=c->last_echo||!newer(f.seq,c->rx_seq)))){c->duplicate++;return false;}c->last_echo=echo;}
 else {if(!c->bound||f.session!=c->session){/* A new ESP transport owner must first submit an actual STOP/disarm frame. */if(!(f.data[3]&1)){c->rejected++;return false;}c->session=f.session;c->tx_seq=0;c->seen=false;c->bound=true;}if(c->seen&&!newer(f.seq,c->rx_seq)){c->duplicate++;return false;}c->echo=f.seq;}
 c->seen=true;c->rx_seq=f.seq;*n=f.data[2];memcpy(out,f.data+8,*n);return true;}
