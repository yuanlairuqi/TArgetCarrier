#include "wave_store.h"
#include "sd_geek.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_vfs_fat.h"
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#ifndef CAP_SD_BASE
#define CAP_SD_BASE "/capture_sd"
#endif
#define BASE CAP_SD_BASE
#define DIRPATH BASE "/tcwave"
#define LIMIT (64U*1024U*1024U)
/* GU kind 6 RC1 request, kind 7 RS1 response. No movement commands here.
 * Request: op3, nonce4, record id8, offset/index12, length16, data20..63.
 * Reply: op3, nonce4, error8, flags9, length10, offset12, id16, data20..63.
 * flags: mounted=1 recording=2; errors: 1 no card,2 IO,3 busy,4 bad request,
 * 5 missing,6 full,7 segment limit,8 duplicate/mismatched nonce.
 */
typedef struct {uint64_t us;uint8_t body[64];} frame_t;
static QueueHandle_t frames,commands,replies;
static atomic_bool accepting;
static atomic_uint dropped;
static FILE *file;
static bool mounted;
static uint8_t fault;
static uint32_t current_id,bytes_written;
static uint64_t start_utc,origin_us,recording_id;
static uint32_t segment_index;
static bool rolling,stopping;
static uint8_t start(const uint8_t*q);
static uint32_t g32(const uint8_t*p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static void p32(uint8_t*p,uint32_t v){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void p64(uint8_t*p,uint64_t v){for(unsigned i=0;i<8;i++)p[i]=(uint8_t)(v>>(8*i));}
static uint64_t g64(const uint8_t*p){return g32(p)|((uint64_t)g32(p+4)<<32);}
static uint32_t crc(const uint8_t*p,size_t n){uint32_t x=~0u;while(n--){x^=*p++;for(unsigned i=0;i<8;i++)x=(x>>1)^((0u-(x&1))&0xedb88320u);}return ~x;}
static void path(char *out,uint32_t id,const char*ext){snprintf(out,80,DIRPATH "/%08lx.%s",(unsigned long)id,ext);}
static void close_segment(void){if(file){if(fflush(file)||fsync(fileno(file)))fault=2;if(fclose(file))fault=2;file=NULL;}}
static void close_record(void){atomic_store(&accepting,false);close_segment();}
static void fail(uint8_t code){fault=code;close_record();if(fault==2){(void)cap_sd_unmount();mounted=false;}}
static bool append(const frame_t*v){
 if(!file){return false;}
 if(bytes_written+80>LIMIT){
  uint8_t q[64]={0};
  /* Keep accepting into the bounded input queue while SD changes segments. */
  close_segment();if(fault==2){fail(2);return false;}
  rolling=true;uint8_t rc=start(q);rolling=false;
  if(rc){fail(rc);return false;}
 }
 uint8_t raw[80];p64(raw,v->us);memcpy(raw+8,v->body,64);p32(raw+72,atomic_load(&dropped));p32(raw+76,crc(raw,76));
 if(fwrite(raw,1,80,file)!=80){fail(2);return false;}bytes_written+=80;return true;
}
static uint8_t mount_card(void){if(mounted){return 0;}if(cap_sd_mount()!=ESP_OK){return fault=1;}
 mounted=true;struct stat st;if(stat(DIRPATH,&st)){if(mkdir(DIRPATH,0777)){fail(2);return 2;}}else if(!S_ISDIR(st.st_mode)){fail(2);return 2;}fault=0;return 0;}
static void status(uint8_t *r){r[9]=(mounted?1:0)|(file?2:0);p32(r+16,current_id);r[10]=24;
 p32(r+20,atomic_load(&dropped));p32(r+24,bytes_written);p64(r+28,start_utc);r[36]=fault;}
static uint32_t file_id(const char*n){if(strlen(n)!=12||strcmp(n+8,".twl")){return 0;}
 for(unsigned i=0;i<8;i++)if(!((n[i]>='0'&&n[i]<='9')||(n[i]>='a'&&n[i]<='f'))){return 0;}
 return (uint32_t)strtoul(n,NULL,16);}
static uint8_t start(const uint8_t*q){
 if(file){return 3;}if(mount_card()){return fault;}
 uint64_t total,free_bytes;if(esp_vfs_fat_info(BASE,&total,&free_bytes)!=ESP_OK){return 2;}if(free_bytes<1024*1024){return fault=6;}
 DIR*d=opendir(DIRPATH);if(!d){return 2;}struct dirent*e;uint32_t max_id=0;while((e=readdir(d))){uint32_t id=file_id(e->d_name);if(id>max_id)max_id=id;}closedir(d);if(max_id==UINT32_MAX){return 7;}
 /* Only worker creates files. Never overwrite an existing id or orphan note. */
 char name[80];uint32_t id=max_id;struct stat st;
 do{if(id==UINT32_MAX){return 7;}id++;path(name,id,"note");}while(stat(name,&st)==0);
 path(name,id,"twl");file=fopen(name,"wb");if(!file){return 2;}
 current_id=id;bytes_written=48;
 if(!rolling){
  start_utc=g64(q+20);origin_us=(uint64_t)esp_timer_get_time();segment_index=0;
  recording_id=((uint64_t)esp_random()<<32)|esp_random();if(!recording_id)recording_id=1;
  atomic_store(&dropped,0);xQueueReset(frames);
 }else{++segment_index;}
 fault=0;
 /* TCWL/2 keeps a stable recording ID and clock origin across segments. */
 uint8_t header[48]={ 'T','C','W','L',2,0,0,0 };p64(header+8,start_utc);p32(header+16,id);p64(header+20,origin_us);
 p64(header+28,recording_id);p32(header+36,segment_index);p32(header+44,crc(header,44));
 if(fwrite(header,1,48,file)!=48||fflush(file)||fsync(fileno(file))){fail(2);return 2;}
 atomic_store(&accepting,!stopping);return 0;
}
static uint8_t execute(const uint8_t*q,uint8_t*r){
 uint8_t op=q[3];uint32_t id=g32(q+8),offset=g32(q+12);char name[80];
 if(q[16]>44){return 4;}
 if(op==1){status(r);return 0;}
 if(op==2){uint8_t rc=start(q);status(r);return rc;}
 if(op==3){stopping=true;atomic_store(&accepting,false);frame_t v;for(unsigned i=0;i<64&&xQueueReceive(frames,&v,0)==pdTRUE;i++)if(file)append(&v);close_record();stopping=false;status(r);return fault==2?2:0;}
 if(op==8){
  if(file){return 3;}
  /* Always clean up adapter resources, even after a previous failed unmount. */
  mounted=false;
  if(cap_sd_unmount()!=ESP_OK){fault=2;status(r);return 2;}
  uint8_t rc=mount_card();status(r);return rc;
 }
 if(!mounted){return 1;}
 if(file){return 3;} /* browsing/downloads do not compete with recording writes */
 if(op==4){
  DIR*d=opendir(DIRPATH);if(!d){return 2;}struct dirent*e;uint32_t index=0,chosen=0;
  /* Cursor is record id, independent of filesystem directory iteration order. */
  while((e=readdir(d))){uint32_t candidate=file_id(e->d_name);if(candidate>offset&&(!chosen||candidate<chosen))chosen=candidate;index++;}closedir(d);(void)index;
  if(!chosen){r[10]=0;return 0;}path(name,chosen,"twl");FILE*f=fopen(name,"rb");if(!f){return 2;}
  uint8_t h[48]={0};size_t got=fread(h,1,32,f);size_t header_size=h[4]==2?48:32;
  if(got==32&&header_size==48)got+=fread(h+32,1,16,f);
  if(fseek(f,0,SEEK_END)){fclose(f);return 2;}long size=ftell(f);fclose(f);
  if(got!=header_size||memcmp(h,"TCWL",4)||(h[4]!=1&&h[4]!=2)||h[5]||h[6]||h[7]||
     crc(h,header_size-4)!=g32(h+header_size-4)||size<(long)header_size){return 2;}
  p32(r+16,chosen);p32(r+12,chosen);p32(r+20,(uint32_t)size);memcpy(r+24,h+8,8);r[10]=12;return 0;
 }
 if(!id){return 4;}path(name,id,"twl");struct stat st;if(stat(name,&st)){return 5;}
 p32(r+16,id);p32(r+12,offset);
 if(op==5||op==9){ /* read original bytes / read note */
  if(op==9){path(name,id,"note");}FILE*f=fopen(name,"rb");if(!f){return 5;}
  if(offset>LIMIT||fseek(f,(long)offset,SEEK_SET)){fclose(f);return 4;}
  r[10]=(uint8_t)fread(r+20,1,44,f);uint8_t rc=ferror(f)?2:0;fclose(f);return rc;
 }
 if(op==6){path(name,id,"note");FILE*f=fopen(name,"wb");if(!f){return 2;}
  size_t n=q[16];bool ok=fwrite(q+20,1,n,f)==n;if(fflush(f)||fsync(fileno(f)))ok=false;if(fclose(f))ok=false;return ok?0:2;}
 if(op==7){if(remove(name)){return 2;}path(name,id,"note");(void)remove(name);return 0;}
 return 4;
}
static void worker(void*arg){
 (void)arg;(void)mount_card();uint32_t flush_at=0,report_at=0;
 /* Request replay cache prevents delayed/double START/DELETE execution. */
 uint8_t saved_q[8][64]={{0}},saved_r[8][64]={{0}};unsigned next=0;
 for(;;){uint32_t now=(uint32_t)(esp_timer_get_time()/1000);uint8_t q[64],r[64]={ 'R','S',1 };
  if(uxQueueSpacesAvailable(replies)&&xQueueReceive(commands,q,0)==pdTRUE){
   r[3]=q[3];memcpy(r+4,q+4,4);bool duplicate=false;
   for(unsigned i=0;i<8;i++)if(g32(saved_q[i]+4)==g32(q+4)&&g32(q+4)){
    if(!memcmp(q,saved_q[i],64))memcpy(r,saved_r[i],64);else r[8]=8;duplicate=true;break;}
   if(!duplicate){r[8]=execute(q,r);r[9]=(mounted?1:0)|(file?2:0);memcpy(saved_q[next],q,64);memcpy(saved_r[next],r,64);next=(next+1)%8;}
   (void)xQueueSend(replies,r,0);
  }
  frame_t v;for(unsigned i=0;i<16&&xQueueReceive(frames,&v,0)==pdTRUE;i++)if(file)append(&v);
  if(file&&now-flush_at>=1000){flush_at=now;
   uint64_t total,free_bytes;if(esp_vfs_fat_info(BASE,&total,&free_bytes)!=ESP_OK)fail(2);
   else if(free_bytes<1024*1024)fail(6);
   else if(fflush(file)||fsync(fileno(file)))fail(2);
  }
  if(now-report_at>=1000&&uxQueueSpacesAvailable(replies)>1){report_at=now;memset(r,0,64);memcpy(r,"RS\1\1",4);status(r);(void)xQueueSend(replies,r,0);}
  vTaskDelay(pdMS_TO_TICKS(10)>0?pdMS_TO_TICKS(10):1);
 }
}
bool wave_store_init(void){frames=xQueueCreate(64,sizeof(frame_t));commands=xQueueCreate(4,64);replies=xQueueCreate(8,64);
 if(!frames||!commands||!replies){return false;}return xTaskCreate(worker,"wave_sd",6144,NULL,1,NULL)==pdPASS;}
void wave_store_frame(const uint8_t body[64],uint64_t us){if(!atomic_load(&accepting)||!frames)return;
 frame_t v={.us=us};memcpy(v.body,body,64);if(xQueueSend(frames,&v,0)!=pdTRUE)atomic_fetch_add(&dropped,1);}
bool wave_store_request(const uint8_t q[64]){return commands&&q[0]=='R'&&q[1]=='C'&&q[2]==1&&g32(q+4)&&xQueueSend(commands,q,0)==pdTRUE;}
bool wave_store_response(uint8_t r[64]){return replies&&xQueueReceive(replies,r,0)==pdTRUE;}
