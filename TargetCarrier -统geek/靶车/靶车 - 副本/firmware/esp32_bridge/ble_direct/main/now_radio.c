#include "now_radio.h"
#include "sdkconfig.h"
#include "esp_log.h"
#ifdef CONFIG_ROBOT_ESPNOW_ENABLED
#include <string.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_now.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "psa/crypto.h"
#define AUTH_ALG PSA_ALG_TRUNCATED_MAC(PSA_ALG_HMAC(PSA_ALG_SHA_256),16)
#ifdef DUAL_NOW_MASTER
#define PEERS 2
#else
#define PEERS 1
#endif
typedef struct {uint8_t data[80];uint32_t at;} item_t;
typedef struct {uint8_t mac[6],lmk[16];uint32_t node;psa_key_id_t key;QueueHandle_t rx,wave_rx,stop_rx,tx,wave_tx,stop_tx;uint32_t last_wave;} peer_t;
static peer_t peers[PEERS];static uint8_t local[6],pmk[16],packet[80];
static atomic_bool ready,sending;static bool recovering;static uint32_t sent_at,recovery_at;static unsigned rr;
static atomic_uint submitted,link_ok,link_fail,rejected,expired,control_drop,rx_drop,wave_drop,rx_high,tx_high,callback_timeout,recoveries,auth_fail;
static uint32_t ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static uint32_t u32(const uint8_t *p){return (uint32_t)p[0]|(uint32_t)p[1]<<8|(uint32_t)p[2]<<16|(uint32_t)p[3]<<24;}
static bool safety(const uint8_t *p){return (p[3]==2&&p[25]==0)||(p[3]==5&&p[24]==0xaa&&p[26]==1&&(!(p[28]&1)||(p[28]&12)));}
static void high(atomic_uint *p,unsigned v){unsigned old=atomic_load(p);if(v>old)atomic_store(p,v);}
void now_radio_stats(now_radio_stats_t *o){*o=(now_radio_stats_t){atomic_load(&submitted),atomic_load(&link_ok),atomic_load(&link_fail),atomic_load(&rejected),atomic_load(&expired),atomic_load(&control_drop),atomic_load(&rx_drop),atomic_load(&wave_drop),atomic_load(&rx_high),atomic_load(&tx_high),atomic_load(&callback_timeout),atomic_load(&recoveries),atomic_load(&auth_fail)};}
static int nibble(char c){if(c>='0'&&c<='9')return c-'0';if(c>='a'&&c<='f')return c-'a'+10;if(c>='A'&&c<='F')return c-'A'+10;return -1;}
static bool hex(const char *s,uint8_t *p,int n,bool mac){if(strlen(s)!=(size_t)(mac?n*3-1:n*2))return false;bool nz=false;for(int i=0;i<n;i++){int at=i*(mac?3:2),a=nibble(s[at]),b=nibble(s[at+1]);if(a<0||b<0||(mac&&i<n-1&&s[at+2]!=':'))return false;p[i]=(uint8_t)(a*16+b);nz|=p[i]!=0;}return nz;}
static bool provision(peer_t *p,uint32_t node,const char *mac,const char *lmk,const char *auth){uint8_t key[32];p->node=node;if(!hex(mac,p->mac,6,true)||(p->mac[0]&1)||!hex(lmk,p->lmk,16,false)||!hex(auth,key,32,false))return false;psa_key_attributes_t a=PSA_KEY_ATTRIBUTES_INIT;psa_set_key_usage_flags(&a,PSA_KEY_USAGE_SIGN_MESSAGE|PSA_KEY_USAGE_VERIFY_MESSAGE);psa_set_key_algorithm(&a,AUTH_ALG);psa_set_key_type(&a,PSA_KEY_TYPE_HMAC);psa_set_key_bits(&a,256);psa_status_t rc=psa_import_key(&a,key,32,&p->key);psa_reset_key_attributes(&a);memset(key,0,32);return rc==PSA_SUCCESS;}
static peer_t *by_node(uint32_t node){for(unsigned i=0;i<PEERS;i++)if(peers[i].node==node)return &peers[i];return NULL;}
static void send_cb(const esp_now_send_info_t *info,esp_now_send_status_t status){(void)info;if(status==ESP_NOW_SEND_SUCCESS)atomic_fetch_add(&link_ok,1);else atomic_fetch_add(&link_fail,1);atomic_store(&sending,false);}
static void recv_cb(const esp_now_recv_info_t *info,const uint8_t *data,int len){
 if(!atomic_load(&ready)||!info||!info->src_addr||!info->des_addr||!data||len!=80||memcmp(info->des_addr,local,6))return;
 for(unsigned i=0;i<PEERS;i++){peer_t *p=&peers[i];if(memcmp(info->src_addr,p->mac,6)||u32(data+4)!=p->node)continue;
  item_t v={.at=ms()};memcpy(v.data,data,80);QueueHandle_t q=data[3]==4?p->wave_rx:safety(data)?p->stop_rx:p->rx;
  /* Callback copies bounded data only; authentication and application work stay in worker. */
  if(q==p->stop_rx){xQueueOverwrite(q,&v);return;}if(xQueueSend(q,&v,0)!=pdTRUE)atomic_fetch_add(data[3]==4?&wave_drop:&rx_drop,1);else high(&rx_high,uxQueueMessagesWaiting(q));return;
 }
}
static esp_err_t start_now(void){esp_err_t rc=esp_now_init();if(rc==ESP_OK)rc=esp_now_set_pmk(pmk);for(unsigned i=0;i<PEERS&&rc==ESP_OK;i++){esp_now_peer_info_t p={.channel=CONFIG_ROBOT_ESPNOW_CHANNEL,.ifidx=WIFI_IF_STA,.encrypt=true};memcpy(p.peer_addr,peers[i].mac,6);memcpy(p.lmk,peers[i].lmk,16);rc=esp_now_add_peer(&p);}if(rc==ESP_OK)rc=esp_now_register_send_cb(send_cb);if(rc==ESP_OK)rc=esp_now_register_recv_cb(recv_cb);return rc;}
bool now_radio_init(void){
 if(psa_crypto_init()!=PSA_SUCCESS||!hex(CONFIG_ROBOT_ESPNOW_PMK,pmk,16,false))return false;
 if(!provision(&peers[0],CONFIG_ROBOT_ESPNOW_NODE_ID,CONFIG_ROBOT_ESPNOW_CONTROLLER_MAC,CONFIG_ROBOT_ESPNOW_LMK,CONFIG_ROBOT_ESPNOW_AUTH_KEY))return false;
#ifdef DUAL_NOW_MASTER
 if(CONFIG_ROBOT_ESPNOW_NODE_ID!=1||!provision(&peers[1],2,CONFIG_ROBOT_ESPNOW_TARGET_MAC,CONFIG_ROBOT_ESPNOW_TARGET_LMK,CONFIG_ROBOT_ESPNOW_TARGET_AUTH_KEY)||!memcmp(peers[0].mac,peers[1].mac,6)||!memcmp(peers[0].lmk,peers[1].lmk,16)||peers[0].key==peers[1].key)return false;
#endif
 for(unsigned i=0;i<PEERS;i++){peer_t *p=&peers[i];p->rx=xQueueCreate(8,sizeof(item_t));p->wave_rx=xQueueCreate(8,sizeof(item_t));p->stop_rx=xQueueCreate(1,sizeof(item_t));p->tx=xQueueCreate(8,sizeof(item_t));p->wave_tx=xQueueCreate(1,sizeof(item_t));p->stop_tx=xQueueCreate(1,sizeof(item_t));if(!p->rx||!p->wave_rx||!p->stop_rx||!p->tx||!p->wave_tx||!p->stop_tx)return false;}
 ESP_ERROR_CHECK(esp_netif_init());ESP_ERROR_CHECK(esp_event_loop_create_default());wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();ESP_ERROR_CHECK(esp_wifi_init(&cfg));ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));ESP_ERROR_CHECK(esp_wifi_start());ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ROBOT_ESPNOW_CHANNEL,WIFI_SECOND_CHAN_NONE));ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA,local));for(unsigned i=0;i<PEERS;i++)if(!memcmp(local,peers[i].mac,6))return false;ESP_ERROR_CHECK(start_now());atomic_store(&ready,true);return true;
}
static bool raw_receive(uint8_t out[64],uint32_t *at){if(!atomic_load(&ready))return false;
 /* Fair per-peer queues; all safety/control is visited before waveform. */
 for(unsigned cls=0;cls<3;cls++)for(unsigned n=0;n<PEERS;n++){peer_t *p=&peers[(rr+n)%PEERS];QueueHandle_t q=cls==0?p->stop_rx:cls==1?p->rx:p->wave_rx;item_t v;for(unsigned k=0;k<8&&xQueueReceive(q,&v,0)==pdTRUE;k++){
 if(ms()-v.at>100&&!safety(v.data)){atomic_fetch_add(&expired,1);continue;}
 if(psa_mac_verify(p->key,AUTH_ALG,v.data,64,v.data+64,16)!=PSA_SUCCESS){atomic_fetch_add(&auth_fail,1);continue;}
 if(v.data[0]!='W'||v.data[1]!='N'||v.data[2]!=1||u32(v.data+4)!=p->node||u32(v.data+60)!=now_crc32(v.data,60))continue;
 memcpy(out,v.data,64);*at=v.at;rr=(unsigned)(p-peers+1)%PEERS;return true;}}
 return false;
}
bool now_radio_receive_frame(uint8_t out[64],uint32_t *at){return raw_receive(out,at);}
bool now_radio_receive(now_request_t *q,uint32_t *at){uint8_t b[64];for(unsigned i=0;i<8&&raw_receive(b,at);i++)if(now_decode(b,64,q))return true;return false;}
void now_radio_send(const uint8_t data[64]){if(!atomic_load(&ready))return;peer_t *p=by_node(u32(data+4));if(!p){atomic_fetch_add(&rejected,1);return;}item_t v={.at=ms()};memcpy(v.data,data,64);if(safety(data)){xQueueReset(p->tx);xQueueReset(p->wave_tx);xQueueOverwrite(p->stop_tx,&v);return;}if(xQueueSend(p->tx,&v,0)!=pdTRUE)atomic_fetch_add(&control_drop,1);else high(&tx_high,uxQueueMessagesWaiting(p->tx));}
bool now_radio_send_wave(const uint8_t data[64]){peer_t *p=by_node(u32(data+4));if(!atomic_load(&ready)||!p||recovering)return false;item_t v={.at=ms()};memcpy(v.data,data,64);return uxQueueMessagesWaiting(p->tx)==0&&uxQueueMessagesWaiting(p->stop_tx)==0&&xQueueSend(p->wave_tx,&v,0)==pdTRUE;}
void now_radio_flush_node(uint32_t node){peer_t *p=by_node(node);if(p&&p->tx){xQueueReset(p->tx);xQueueReset(p->wave_tx);xQueueReset(p->stop_tx);}}
void now_radio_flush(void){for(unsigned i=0;i<PEERS;i++)now_radio_flush_node(peers[i].node);}
void now_radio_service(void){uint32_t now=ms();if(recovering){if(now-recovery_at<NOW_RADIO_RECOVERY_MS)return;if(start_now()!=ESP_OK){(void)esp_now_deinit();recovery_at=now;return;}atomic_store(&sending,false);recovering=false;atomic_store(&ready,true);return;}if(!atomic_load(&ready))return;
 if(atomic_load(&sending)){if(now-sent_at>=NOW_RADIO_CALLBACK_MS){atomic_store(&ready,false);(void)esp_now_unregister_send_cb();(void)esp_now_unregister_recv_cb();(void)esp_now_deinit();now_radio_flush();for(unsigned i=0;i<PEERS;i++){xQueueReset(peers[i].rx);xQueueReset(peers[i].wave_rx);xQueueReset(peers[i].stop_rx);}atomic_fetch_add(&callback_timeout,1);atomic_fetch_add(&recoveries,1);recovering=true;recovery_at=now;}return;}
 for(unsigned cls=0;cls<3;cls++)for(unsigned n=0;n<PEERS;n++){peer_t *p=&peers[(rr+n)%PEERS];if(cls==2&&now-p->last_wave<NOW_WAVE_GAP_MS)continue;QueueHandle_t q=cls==0?p->stop_tx:cls==1?p->tx:p->wave_tx;item_t v;for(unsigned k=0;k<8&&xQueueReceive(q,&v,0)==pdTRUE;k++){if(now-v.at>100&&!safety(v.data)){atomic_fetch_add(&expired,1);continue;}memcpy(packet,v.data,64);size_t len=0;sent_at=now;atomic_store(&sending,true);rr=(unsigned)(p-peers+1)%PEERS;if(cls==2)p->last_wave=now;if(psa_mac_compute(p->key,AUTH_ALG,packet,64,packet+64,16,&len)!=PSA_SUCCESS||len!=16||esp_now_send(p->mac,packet,80)!=ESP_OK){atomic_fetch_add(&rejected,1);atomic_store(&sending,false);}else atomic_fetch_add(&submitted,1);return;}}
}
#else
void now_radio_stats(now_radio_stats_t *o){*o=(now_radio_stats_t){0};}
bool now_radio_init(void){return false;}
bool now_radio_receive(now_request_t *q,uint32_t *at){(void)q;(void)at;return false;}
bool now_radio_receive_frame(uint8_t out[64],uint32_t *at){(void)out;(void)at;return false;}
void now_radio_send(const uint8_t *p){(void)p;}
bool now_radio_send_wave(const uint8_t *p){(void)p;return false;}
void now_radio_service(void){}
void now_radio_flush(void){}
void now_radio_flush_node(uint32_t n){(void)n;}
#endif
