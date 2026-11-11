#include "fragment.h"
#include "batch.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void){cap_batch a={.source=1,.boot=2,.period_us=40000,.type=1,.count=8},decoded;uint8_t b[72],f[20];assert(cap_batch_encode(&a,b,sizeof b)==72);frag_join j={0};const uint8_t*out;size_t n;for(unsigned i=0;i<6;i++){assert(frag_encode(b,72,77,i,f));assert(frag_accept(&j,f,i*20,&out,&n)==(i==5));}assert(n==72&&!memcmp(b,out,n)&&cap_batch_decode(out,n,&decoded));assert(frag_encode(b,72,78,0,f)&&frag_accept(&j,f,0,&out,&n)==0);assert(frag_encode(b,72,78,2,f)&&frag_accept(&j,f,40,&out,&n)==-1);assert(frag_encode(b,72,79,0,f)&&frag_accept(&j,f,0,&out,&n)==0);assert(frag_encode(b,72,79,1,f)&&frag_accept(&j,f,501,&out,&n)==-1);assert(frag_encode(b,72,80,0,f)&&frag_accept(&j,f,0xffffffffu-10,&out,&n)==0);assert(frag_encode(b,72,80,1,f)&&frag_accept(&j,f,10,&out,&n)==0);puts("PASS fragment sequence, missing fragment, timeout, tick wrap and batch CRC");}
