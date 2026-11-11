"""Compile production radio/guard/protocol code with bounded RTOS/MAC test doubles."""
from pathlib import Path
import subprocess,os,shutil,argparse,tempfile,hashlib,json
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--master',type=Path,default=Path(__file__).resolve().parents[1])
parser.add_argument('--slave',type=Path)
parser.add_argument('--output',type=Path)
parser.add_argument('--gcc',type=Path,default=Path(r'D:\mingw\mingw64\bin\gcc.exe'))
args=parser.parse_args()
if args.slave is None:args.slave=args.master.parent/'x轴/esp_无线从机_20261022'
OUT=(args.output or Path(tempfile.mkdtemp(prefix='tc-link-'))).resolve();OUT.mkdir(exist_ok=True,parents=True)
if not str(OUT).isascii():parser.error('Use an ASCII --output path for native GCC')
B=OUT/'sources';manifest={}
for role,root in [('geek',args.master),('slave',args.slave)]:
 target=B/role/'main';target.mkdir(parents=True,exist_ok=True)
 for f in (root/'main').glob('*'):
  if f.suffix in ('.c','.h'):
   shutil.copy2(f,target/f.name);manifest[role+'/'+f.name]=hashlib.sha256(f.read_bytes()).hexdigest()
(OUT/'source-sha256.json').write_text(json.dumps(manifest,indent=2),encoding='utf8')
MOCK=r'''
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
typedef struct {unsigned cap,size,count,head;unsigned char *buf;} mock_queue_t;
typedef mock_queue_t* QueueHandle_t;
#define pdTRUE 1
static inline QueueHandle_t xQueueCreate(unsigned n,unsigned s){QueueHandle_t q=calloc(1,sizeof(*q));q->cap=n;q->size=s;q->buf=calloc(n,s);return q;}
static inline int xQueueSend(QueueHandle_t q,const void*p,int wait){(void)wait;if(q->count==q->cap)return 0;memcpy(q->buf+((q->head+q->count)%q->cap)*q->size,p,q->size);q->count++;return 1;}
static inline int xQueueReceive(QueueHandle_t q,void*p,int wait){(void)wait;if(!q->count)return 0;memcpy(p,q->buf+q->head*q->size,q->size);q->head=(q->head+1)%q->cap;q->count--;return 1;}
static inline void xQueueReset(QueueHandle_t q){q->head=q->count=0;}
static inline unsigned uxQueueMessagesWaiting(QueueHandle_t q){return q->count;}
static inline int xQueueOverwrite(QueueHandle_t q,const void*p){xQueueReset(q);return xQueueSend(q,p,0);}
extern uint32_t mock_ms;extern int mock_deinit,mock_send_result;extern const uint8_t *mock_inflight;
static inline int64_t esp_timer_get_time(void){return (int64_t)mock_ms*1000;}
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_OK 0
#define ESP_ERROR_CHECK(x) do{if((x)!=0)abort();}while(0)
typedef int esp_err_t;
typedef struct {int dummy;} esp_now_send_info_t;
typedef enum {ESP_NOW_SEND_SUCCESS,ESP_NOW_SEND_FAIL} esp_now_send_status_t;
typedef struct {const uint8_t *src_addr,*des_addr;} esp_now_recv_info_t;
typedef struct {int channel,ifidx;bool encrypt;uint8_t peer_addr[6],lmk[16];} esp_now_peer_info_t;
typedef struct {int dummy;} wifi_init_config_t;
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 0
#define WIFI_PS_NONE 0
#define WIFI_IF_STA 0
#define WIFI_SECOND_CHAN_NONE 0
static inline int esp_netif_init(void){return 0;}
static inline int esp_event_loop_create_default(void){return 0;}
static inline int esp_wifi_init(const wifi_init_config_t*p){(void)p;return 0;}
static inline int esp_wifi_set_storage(int x){(void)x;return 0;}
static inline int esp_wifi_set_mode(int x){(void)x;return 0;}
static inline int esp_wifi_start(void){return 0;}
static inline int esp_wifi_set_ps(int x){(void)x;return 0;}
static inline int esp_wifi_set_channel(int a,int b){(void)a;(void)b;return 0;}
static inline int esp_wifi_get_mac(int x,uint8_t*p){(void)x;uint8_t m[6]={2,1,2,3,4,6};memcpy(p,m,6);return 0;}
static inline int esp_now_init(void){return 0;}
static inline int esp_now_deinit(void){mock_deinit++;return 0;}
static inline int esp_now_set_pmk(const uint8_t*p){(void)p;return 0;}
static inline int esp_now_add_peer(const esp_now_peer_info_t*p){(void)p;return 0;}
static inline int esp_now_register_send_cb(void(*f)(const esp_now_send_info_t*,esp_now_send_status_t)){(void)f;return 0;}
static inline int esp_now_register_recv_cb(void(*f)(const esp_now_recv_info_t*,const uint8_t*,int)){(void)f;return 0;}
static inline int esp_now_unregister_send_cb(void){return 0;}
static inline int esp_now_unregister_recv_cb(void){return 0;}
static inline int esp_now_send(const uint8_t*p,const uint8_t*d,size_t n){(void)p;(void)n;mock_inflight=d;return mock_send_result;}
typedef unsigned psa_key_id_t;typedef int psa_status_t;typedef int psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT 0
#define PSA_KEY_USAGE_SIGN_MESSAGE 1
#define PSA_KEY_USAGE_VERIFY_MESSAGE 2
#define PSA_KEY_TYPE_HMAC 1
#define PSA_ALG_SHA_256 1
#define PSA_ALG_HMAC(x) (x)
#define PSA_ALG_TRUNCATED_MAC(x,y) (x)
#define PSA_SUCCESS 0
static inline void psa_set_key_usage_flags(int*a,int b){(void)a;(void)b;}
static inline void psa_set_key_algorithm(int*a,int b){(void)a;(void)b;}
static inline void psa_set_key_type(int*a,int b){(void)a;(void)b;}
static inline void psa_set_key_bits(int*a,int b){(void)a;(void)b;}
static inline void psa_reset_key_attributes(int*a){(void)a;}
static inline int psa_crypto_init(void){return 0;}
static inline int psa_import_key(const int*a,const uint8_t*b,size_t n,unsigned*k){(void)a;(void)b;(void)n;*k=1;return 0;}
static inline int psa_mac_verify(unsigned k,int a,const uint8_t*d,size_t n,const uint8_t*t,size_t m){(void)k;(void)a;(void)d;(void)n;(void)t;(void)m;return 0;}
static inline int psa_mac_compute(unsigned k,int a,const uint8_t*d,size_t n,uint8_t*t,size_t m,size_t*l){(void)k;(void)a;(void)d;(void)n;(void)m;memset(t,0,16);*l=16;return 0;}
'''
TEST=r'''
#include "mock.h"
#include <assert.h>
#include <stdio.h>
uint32_t mock_ms;int mock_deinit,mock_send_result;const uint8_t *mock_inflight;
#include "now_radio.c"
#include "master_guard.h"
#include "usb_wire.h"
static void crc(uint8_t *b){uint32_t c=now_crc32(b,60);memcpy(b+60,&c,4);}
static void body(uint8_t*b,int kind,int op,unsigned seq){memset(b,0,80);b[0]='W';b[1]='N';b[2]=1;b[3]=kind;b[4]=1;b[24]=4;b[25]=op;b[26]=op?40:0;memcpy(b+28,&seq,4);crc(b);}
static void receive(uint8_t*b){esp_now_recv_info_t i={peer,local};receive_cb(&i,b,80);}
int main(void){
 assert(now_radio_init());uint8_t b[80],out[64];uint32_t at;
 body(b,4,0,0);for(int i=0;i<500;i++)receive(b);
 body(b,3,0,0);receive(b);
 assert(now_radio_receive_frame(out,&at)&&out[3]==3);assert(uxQueueMessagesWaiting(wave_rx)==16);
 puts("PASS 500 wave arrivals cannot occupy the control RX queue; bounded wave drops");
 xQueueReset(queue);xQueueReset(wave_rx);
 body(b,2,1,1);for(int i=0;i<20;i++)receive(b);
 body(b,2,0,30);receive(b);assert(now_radio_receive_frame(out,&at)&&out[25]==0);
 puts("PASS hard STOP reserved RX slot wins over a full command queue");
 xQueueReset(queue);body(b,3,0,0);receive(b);mock_ms=101;assert(!now_radio_receive_frame(out,&at));
 body(b,2,0,30);receive(b);mock_ms+=101;assert(now_radio_receive_frame(out,&at)&&out[25]==0);
 puts("PASS stale buffered control/status discarded; old STOP reaches protocol without renewal");
 body(b,2,1,1);now_radio_send(b);now_radio_service();assert(atomic_load(&sending));
 uint8_t saved[80];memcpy(saved,mock_inflight,80);body(b,2,1,2);now_radio_send(b);mock_ms+=20;now_radio_service();assert(!memcmp(saved,mock_inflight,80));
 puts("PASS in-flight MAC buffer retained until callback, subsequent sends wait");
 body(b,2,0,3);now_radio_send(b);assert(uxQueueMessagesWaiting(control_tx)==1);
 send_cb(NULL,ESP_NOW_SEND_FAIL);now_radio_service();assert(mock_inflight[25]==0);
 puts("PASS STOP flushes pending movement; MAC failure never retries movement");
 mock_ms+=251;now_radio_service();assert(mock_deinit==1&&!ready&&recovering);assert(!uxQueueMessagesWaiting(control_tx));
 mock_ms+=1001;now_radio_service();assert(ready&&!recovering&&!atomic_load(&sending));
 now_radio_stats_t stats;now_radio_stats(&stats);assert(stats.callback_timeout==1&&stats.recoveries==1);
 puts("PASS missing MAC callback tears down driver and clears backlog before timed recovery");
 body(b,4,0,0);assert(now_radio_send_wave(b));now_radio_service();send_cb(NULL,ESP_NOW_SEND_SUCCESS);
 assert(now_radio_send_wave(b));mock_ms+=10;now_radio_service();assert(!atomic_load(&sending));
 body(b,3,0,0);now_radio_send(b);now_radio_service();assert(atomic_load(&sending)&&mock_inflight[3]==3);send_cb(NULL,ESP_NOW_SEND_SUCCESS);
 puts("PASS wave limit applies only to waves; control bypasses wave token wait");
 master_guard_t g={0};now_request_t q={.boot=7,.epoch=1,.session=10,.seq=1};q.command[1]=0;
 assert(master_guard_status(&g,7,1,10));assert(!master_guard_command(&g,&q,10));master_guard_query(&g,10);
 assert(master_guard_command(&g,&q,10));q.command[1]=1;q.seq=2;assert(!master_guard_command(&g,&q,10));g.unlocked=true;
 assert(master_guard_command(&g,&q,10));assert(!master_guard_command(&g,&q,11));
 assert(master_guard_expired(&g,true,611));master_guard_loss(&g);master_guard_query(&g,612);master_guard_status(&g,7,1,612);
 assert(!master_guard_command(&g,&q,612));q.command[1]=0;assert(!master_guard_command(&g,&q,612));
 q.session=20;assert(master_guard_command(&g,&q,612));q.command[1]=1;++q.seq;assert(!master_guard_command(&g,&q,612));
 puts("PASS master expiry rejects old session and duplicate movement; fresh session requires STOP ACK");
 assert(master_guard_status(&g,7,2,613));assert(!master_guard_status(&g,7,1,614));assert(!g.unlocked);
 puts("PASS delayed old challenge cannot roll the master back after recovery");
 uint8_t status[64]={0},cmd[64]={0};status[0]=cmd[0]='W';status[1]=cmd[1]='N';status[2]=cmd[2]=1;status[3]=3;cmd[3]=2;
 status[4]=cmd[4]=1;status[8]=cmd[8]=7;status[12]=cmd[12]=1;status[16]=cmd[16]=9;status[20]=cmd[20]=3;
 cmd[24]=4;cmd[28]=3;cmd[36]=cmd[38]=200;status[25]=2;status[40]=4;status[42]=185;status[48]=status[52]=3;crc(status);crc(cmd);
 assert(master_output_confirmed(status,cmd));status[43]=20;crc(status);assert(!master_output_confirmed(status,cmd));
 puts("PASS output STOP confirmation requires actual zero frequency, not only radio ACK");
 #ifdef TEST_SLAVE
 now_control_t n;robot_control_t r;robot_init(&r);r.uart_configured=true;r.stm_online=true;r.stm_flags=1;
 now_control_init(&n,1,7);q=(now_request_t){.kind=NOW_COMMAND,.node=1,.boot=7,.epoch=1,.session=10,.seq=1};q.command[0]=4;q.command[4]=1;q.command[12]=q.command[14]=200;
 assert(now_control_command(&n,&r,&q,0,0)==NOW_OK);q.seq=2;q.command[4]=2;q.command[1]=1;q.command[2]=40;
 assert(now_control_command(&n,&r,&q,10,10)==NOW_OK);unsigned revision=r.revision;
 assert(now_control_command(&n,&r,&q,20,20)==NOW_REPLAY);assert(r.revision==revision);
 q.command[1]=0;q.command[2]=0;assert(now_control_command(&n,&r,&q,0,111)==NOW_STALE);assert(r.motion==0&&n.last_fresh_ms==10);
 q.seq=3;q.command[4]=3;q.command[1]=1;q.command[2]=40;assert(now_control_command(&n,&r,&q,120,120)==NOW_OK);
 now_control_link_lost(&n,&r);assert(!n.active&&r.motion==0&&n.epoch==2);
 assert(now_control_command(&n,&r,&q,30,30)==NOW_CHALLENGE);
 q.epoch=2;q.session=20;assert(now_control_command(&n,&r,&q,30,30)==NOW_NEED_STOP);
 puts("PASS slave revokes epoch on radio reset; duplicate/delayed movement cannot resume control");
 #endif
 return 0;
}
'''
mock=OUT/'mock';mock.mkdir(exist_ok=True)
(mock/'mock.h').write_text(MOCK,encoding='utf8')
for rel in ['freertos/FreeRTOS.h','freertos/queue.h','esp_log.h','esp_wifi.h','esp_now.h','esp_event.h','esp_netif.h','esp_timer.h','psa/crypto.h']:
 p=mock/rel;p.parent.mkdir(exist_ok=True,parents=True);p.write_text('#include "mock.h"\n',encoding='utf8')
(mock/'sdkconfig.h').write_text('''#define CONFIG_ROBOT_ESPNOW_ENABLED 1
#define CONFIG_ROBOT_ESPNOW_NODE_ID 1
#define CONFIG_ROBOT_ESPNOW_CHANNEL 1
#define CONFIG_ROBOT_ESPNOW_CONTROLLER_MAC "02:01:02:03:04:05"
#define CONFIG_ROBOT_ESPNOW_PMK "01010101010101010101010101010101"
#define CONFIG_ROBOT_ESPNOW_LMK "02020202020202020202020202020202"
#define CONFIG_ROBOT_ESPNOW_AUTH_KEY "0303030303030303030303030303030303030303030303030303030303030303"
''',encoding='utf8')
(OUT/'test.c').write_text(TEST,encoding='utf8')
gcc=args.gcc;env=dict(os.environ,PATH=str(gcc.parent)+os.pathsep+os.environ['PATH'])
logs=[]
for role in ['geek','slave']:
 src=B/role/'main';exe=OUT/f'{role}.exe'
 cmd=[str(gcc),'-std=c11','-Wall','-Wextra','-Werror',f'-I{mock}',f'-I{src}',f'-I{B/"geek/main"}',str(OUT/'test.c'),str(src/'now_protocol.c'),str(src/'robot_control.c'),str(B/'geek/main/master_guard.c'),str(B/'geek/main/usb_wire.c'),'-o',str(exe)]
 if role=='slave':cmd.insert(1,'-DTEST_SLAVE')
 build=subprocess.run(cmd,env=env,capture_output=True,text=True)
 if build.returncode:print(build.stderr);raise SystemExit(build.returncode)
 run=subprocess.run([str(exe)],env=env,capture_output=True,text=True);print(role,run.stdout,run.stderr)
 logs.append(role+'\n'+run.stdout+run.stderr)
 if run.returncode:raise SystemExit(run.returncode)
# Also compile the disabled wireless branch, which must not reference RTOS state.
disabled=OUT/'disabled';disabled.mkdir(exist_ok=True);(disabled/'sdkconfig.h').write_text('',encoding='utf8')
for role in ['geek','slave']:
 cmd=[str(gcc),'-std=c11','-Wall','-Wextra','-Werror',f'-I{disabled}',f'-I{mock}',f'-I{B/role/"main"}','-c',str(B/role/'main/now_radio.c'),'-o',str(OUT/f'{role}-disabled.o')]
 subprocess.run(cmd,env=env,check=True)
logs.append('PASS both disabled radio branches compile')
(OUT/'tests.log').write_text('\n'.join(logs),encoding='utf8')
print('Real radio/guard/protocol C passed; RTOS/WiFi/HMAC are mocked. No hardware evidence.')
