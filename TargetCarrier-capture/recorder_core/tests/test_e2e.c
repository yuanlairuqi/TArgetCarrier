/* C source -> WN fragments -> actual TCWL file -> Python replay in runner. */
#define CAP_SD_BASE "e2e_sd"
#define main storage_fault_test_main
#include "test_storage.c"
#undef main
#include "../source.h"
static uint64_t clock_us;
static bool radio_sink(const uint8_t fragment[20],void*ctx){
 (void)ctx;frame_t v={.us=clock_us};memcpy(v.body,"WN\1\4",4);p32(v.body+4,1);p32(v.body+8,7);p32(v.body+12,3);p32(v.body+16,9);v.body[32]=3;
 memcpy(v.body+40,fragment,20);p32(v.body+60,crc(v.body,60));return append(&v);
}
int main(void){(void)mkdir("e2e_sd");assert(wave_store_init());uint8_t q[64];request(q,2,0,0);p64(q+20,100000);assert(start(q)==0);
 cap_source source;cap_source_init(&source,1,7);
 assert(cap_source_feed(&source,CAP_SAMPLE,1,CAP_U32,1,2100000,123));
 assert(cap_source_feed(&source,CAP_SAMPLE,2,CAP_U32,2,2100000,3300));
 assert(cap_source_feed(&source,CAP_EVENT,100,CAP_U32,0,2100000,9));
 for(clock_us=2100000;clock_us<2900000;clock_us+=10000)cap_source_poll(&source,true,clock_us,radio_sink,NULL);
 assert(!source.queue.count&&!source.pending&&source.lost==0);close_record();puts("C end-to-end file generated PASS");return 0;}
