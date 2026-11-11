#define CAP_SD_BASE "boundary_sd"
#define main prior_storage_test_main
#include "test_storage.c"
#undef main
static unsigned incoming;
static void inject(void){uint8_t b[64]={0};for(unsigned i=0;i<incoming;i++)wave_store_frame(b,3000000+i);}
int main(void){
 (void)mkdir("boundary_sd");assert(wave_store_init());uint8_t q[64],r[64]={0};request(q,2,0,0);assert(execute(q,r)==0);
 uint64_t sid=recording_id,origin=origin_us;assert(sid&&segment_index==0);
 frame_t v={.us=3000000};bytes_written=LIMIT-79;incoming=1;write_hook=inject;assert(append(&v));
 assert(atomic_load(&accepting)&&frames->count==1&&atomic_load(&dropped)==0);
 assert(recording_id==sid&&origin_us==origin&&segment_index==1);frame_t got;assert(xQueueReceive(frames,&got,0)==pdTRUE&&got.us==3000000);
 bytes_written=LIMIT-79;incoming=70;write_hook=inject;assert(append(&v));
 assert(frames->count==64&&atomic_load(&dropped)==6&&recording_id==sid&&segment_index==2);
 request(q,8,0,0);unsigned mounts=mount_calls,unmounts=unmount_calls;assert(execute(q,r)==3&&mount_calls==mounts&&unmount_calls==unmounts);
 request(q,3,0,0);assert(execute(q,r)==0&&!atomic_load(&accepting)&&!file);
 request(q,8,0,0);assert(execute(q,r)==0&&unmount_calls==unmounts+1&&mount_calls==mounts+1&&mounted);
 unmount_failure=1;mounts=mount_calls;assert(execute(q,r)==2&&!mounted&&mount_calls==mounts);
 unmount_failure=0;mount_failure=1;assert(execute(q,r)==1&&!mounted);
 mount_failure=0;assert(execute(q,r)==0&&mounted&&fault==0);
 request(q,2,0,0);assert(execute(q,r)==0&&recording_id!=sid&&segment_index==0);close_record();
 puts("boundary PASS: rollover arrival retained; overflow counted; session stable; remount cleanup/retry/busy");return 0;
}
