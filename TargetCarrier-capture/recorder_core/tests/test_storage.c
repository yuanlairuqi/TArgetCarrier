#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/stat.h>
/* Actual storage worker routines on host filesystem, with fault injection.
 * Does not claim physical FAT or FreeRTOS scheduling coverage. */
static int write_failure;
static void (*write_hook)(void);
static size_t test_write(const void*p,size_t size,size_t n,FILE*f){if(write_failure)return 0;if(write_hook){void(*fn)(void)=write_hook;write_hook=NULL;fn();}return fwrite(p,size,n,f);}
#include <io.h>
static int test_mkdir(const char*p,unsigned mode){(void)mode;return mkdir(p);}
#define mkdir test_mkdir
#define fsync _commit
#define fwrite test_write
#ifndef CAP_SD_BASE
#define CAP_SD_BASE "test_sd"
#endif
#include "../wave_store.c"
#undef fwrite
#undef mkdir
struct mock_queue {unsigned capacity,size,count,head;uint8_t *data;};
QueueHandle_t xQueueCreate(unsigned n,unsigned s){QueueHandle_t q=calloc(1,sizeof(*q));q->capacity=n;q->size=s;q->data=calloc(n,s);return q;}
int xQueueSend(QueueHandle_t q,const void*p,unsigned wait){(void)wait;if(q->count==q->capacity)return 0;memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,p,q->size);q->count++;return 1;}
int xQueueReceive(QueueHandle_t q,void*p,unsigned wait){(void)wait;if(!q->count)return 0;memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;return 1;}
unsigned uxQueueSpacesAvailable(QueueHandle_t q){return q->capacity-q->count;}
void xQueueReset(QueueHandle_t q){q->head=q->count=0;}
int xTaskCreate(void(*fn)(void*),const char*n,unsigned s,void*a,unsigned p,void*t){(void)fn;(void)n;(void)s;(void)a;(void)p;(void)t;return 1;}
void vTaskDelay(unsigned n){(void)n;}
int64_t esp_timer_get_time(void){return 2000000;}
static unsigned mount_calls,unmount_calls;static int unmount_failure;
uint32_t esp_random(void){static uint32_t n=123;return ++n;}
static int mount_failure;static uint64_t free_space=10000000;
esp_err_t cap_sd_mount(void){mount_calls++;return mount_failure?1:0;}
esp_err_t cap_sd_unmount(void){unmount_calls++;return unmount_failure?1:0;}
esp_err_t esp_vfs_fat_info(const char*p,uint64_t*t,uint64_t*f){(void)p;*t=100000000;*f=free_space;return 0;}
static void request(uint8_t*q,unsigned op,uint32_t id,uint32_t offset){memset(q,0,64);memcpy(q,"RC\1",3);q[3]=op;p32(q+4,123);p32(q+8,id);p32(q+12,offset);}
int main(void){
 (void)mkdir("test_sd");assert(wave_store_init());uint8_t q[64],r[64]={0};
 mount_failure=1;request(q,2,0,0);assert(execute(q,r)==1);assert(!file&&!atomic_load(&accepting));mount_failure=0;
 free_space=500;assert(execute(q,r)==6);assert(!file);free_space=10000000;
 assert(execute(q,r)==0);uint32_t id=current_id;assert(file&&atomic_load(&accepting));
 request(q,4,0,0);assert(execute(q,r)==3);request(q,7,id,0);assert(execute(q,r)==3);
 uint8_t body[64]={'W','N',1,3};p32(body+60,crc(body,60));
 for(unsigned i=0;i<70;i++)wave_store_frame(body,i);assert(atomic_load(&dropped)==6);
 request(q,3,0,0);assert(execute(q,r)==0);assert(!file&&!atomic_load(&accepting));assert(bytes_written==48+64*80);
 memset(r,0,64);request(q,4,0,0);assert(execute(q,r)==0&&g32(r+16)==id&&g32(r+20)==bytes_written);
 request(q,5,id,0);assert(execute(q,r)==0&&r[10]==44&&memcmp(r+20,"TCWL\2",5)==0);
 request(q,6,id,0);q[16]=5;memcpy(q+20,"hello",5);assert(execute(q,r)==0);
 request(q,9,id,0);assert(execute(q,r)==0&&r[10]==5&&!memcmp(r+20,"hello",5));
 request(q,2,0,0);assert(execute(q,r)==0);frame_t v={.us=1};memcpy(v.body,body,64);write_failure=1;
 assert(!append(&v)&&!file&&!atomic_load(&accepting)&&fault==2&&!mounted);write_failure=0;
 assert(mount_card()==0);request(q,2,0,0);assert(execute(q,r)==0);bytes_written=LIMIT-79;uint32_t previous=current_id;assert(append(&v)&&file&&current_id>previous&&bytes_written==128);close_record();
 request(q,7,id,0);assert(execute(q,r)==0);assert(execute(q,r)==5);
 request(q,5,0,0);assert(execute(q,r)==4);request(q,90,id,0);assert(execute(q,r)==5);
 puts("storage: no card/full/queue overflow/start/stop/list/read/note/delete/write fault/segment limit PASS");return 0;
}
