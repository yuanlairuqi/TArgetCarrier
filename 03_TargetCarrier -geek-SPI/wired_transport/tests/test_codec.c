
#include "wl_codec.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){for(unsigned size=17;size<=32;size+=15){uint8_t data[32],out[32],cmd[128],reply[128],old[128];for(unsigned i=0;i<size;i++)data[i]=(uint8_t)(i*7);wl_codec master,slave;wl_codec_init(&master,true,1,123);wl_codec_init(&slave,false,1,0);unsigned n=0;
 assert(wl_encode(&master,data,size,false,1,cmd));assert(!wl_decode(&slave,cmd,out,&n)&&!slave.bound);
 assert(wl_encode(&master,data,size,true,2,cmd));assert(wl_decode(&slave,cmd,out,&n)&&n==size&&!memcmp(out,data,size));assert(!wl_decode(&slave,cmd,out,&n));
 assert(wl_encode(&slave,data,size,false,3,reply));assert(wl_decode(&master,reply,out,&n)&&n==size&&!memcmp(out,data,size));memcpy(old,reply,128);assert(!wl_decode(&master,reply,out,&n));
 for(unsigned i=0;i<128;i++){uint8_t bad[128];memcpy(bad,reply,128);bad[i]^=1;assert(!wl_decode(&master,bad,out,&n));}
 wl_codec_init(&slave,false,1,0);assert(wl_encode(&master,data,size,true,4,cmd));assert(wl_decode(&slave,cmd,out,&n));assert(wl_encode(&slave,data,size,false,5,reply));assert(wl_decode(&master,reply,out,&n));assert(!wl_decode(&master,old,out,&n));
 wl_codec other;wl_codec_init(&other,true,2,123);other.tx_seq=master.tx_seq;assert(!wl_decode(&other,reply,out,&n));wl_codec_init(&other,true,1,456);assert(!wl_decode(&other,reply,out,&n));
 assert(wl_encode(&other,data,size,false,6,cmd));assert(!wl_decode(&slave,cmd,out,&n));assert(wl_encode(&other,data,size,true,7,cmd));assert(wl_decode(&slave,cmd,out,&n));assert(wl_encode(&slave,data,size,false,8,reply));assert(wl_decode(&other,reply,out,&n));
 other.tx_seq=UINT32_MAX;assert(wl_encode(&other,data,size,true,9,cmd));assert(tc_get32(cmd+8)==1);
 }puts("PASS legacy 17/32-byte roundtrip; STOP-first owner; CRC; duplicate/old session; stale feedback; STM reset echo; sequence wrap");return 0;}
