#include "batch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){cap_batch a={.source=2,.boot=8,.first_seq=0xfffffffc,.first_us=0x1122334455667788ULL,.period_us=40000,.channel=3,.unit=7,.type=3,.count=8},b;uint8_t out[72];for(unsigned i=0;i<8;i++)a.values[i]=0x3f800000u+i;assert(cap_batch_encode(&a,out,71)==0);assert(cap_batch_encode(&a,out,72)==72);assert(cap_batch_decode(out,72,&b));assert(b.first_us==a.first_us&&b.first_seq==a.first_seq&&b.count==8&&b.values[7]==a.values[7]);for(unsigned i=0;i<72;i++){out[i]^=1;assert(!cap_batch_decode(out,72,&b));out[i]^=1;}for(size_t i=0;i<72;i++)assert(!cap_batch_decode(out,i,&b));a.first_us=UINT64_MAX;assert(!cap_batch_encode(&a,out,72));a.count=0;assert(!cap_batch_encode(&a,out,72));puts("PASS batch codec, bounds, CRC, timestamp, seq wrap");}
