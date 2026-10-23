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
#define RADIO_SIZE (NOW_FRAME_SIZE+16)
#define AUTH_ALG PSA_ALG_TRUNCATED_MAC(PSA_ALG_HMAC(PSA_ALG_SHA_256),16)
static psa_key_id_t auth_key;
typedef struct { uint8_t data[RADIO_SIZE];uint32_t at; } received_t;
static QueueHandle_t queue;
static uint8_t peer[6],local[6];
static atomic_bool ready;
static atomic_bool sending;
typedef struct {uint8_t data[64];uint32_t at;} tx_item_t;
static QueueHandle_t control_tx, wave_tx;
static atomic_uint submitted,link_ok,link_fail,rejected,expired,control_drop;
void now_radio_stats(now_radio_stats_t *out){
    *out=(now_radio_stats_t){atomic_load(&submitted),atomic_load(&link_ok),atomic_load(&link_fail),
        atomic_load(&rejected),atomic_load(&expired),atomic_load(&control_drop)};
}
static bool raw_receive(uint8_t out[64],uint32_t *received);
static void send_cb(const esp_now_send_info_t *info,esp_now_send_status_t status) {
    (void)info;if(status==ESP_NOW_SEND_SUCCESS)atomic_fetch_add(&link_ok,1);else atomic_fetch_add(&link_fail,1);atomic_store(&sending,false);
}
static int nibble(char c) {
    if(c>='0'&&c<='9')return c-'0';
    if(c>='a'&&c<='f')return c-'a'+10;
    if(c>='A'&&c<='F')return c-'A'+10;
    return -1;
}
static bool parse_hex(const char *s,uint8_t *out,int n,bool mac) {
    if(strlen(s)!=(size_t)(mac?n*3-1:n*2))return false;
    bool nonzero=false;
    for(int i=0;i<n;++i){
        int offset=i*(mac?3:2),a=nibble(s[offset]),b=nibble(s[offset+1]);
        if(a<0||b<0||(mac&&i<n-1&&s[offset+2]!=':'))return false;
        out[i]=(uint8_t)(a*16+b);nonzero|=out[i]!=0;
    }
    return nonzero;
}
static void receive_cb(const esp_now_recv_info_t *info,const uint8_t *data,int len) {
    // Wi-Fi task context: bounded copy only. No motor, UART, BLE, logging or waits here.
    if(!ready||!info||!info->src_addr||!info->des_addr||!data||len!=RADIO_SIZE||
       memcmp(info->src_addr,peer,6)||memcmp(info->des_addr,local,6))return;
    received_t item={.at=(uint32_t)(esp_timer_get_time()/1000)};
    memcpy(item.data,data,RADIO_SIZE);
    // Queue-full packets are dropped; they cannot renew the application lease.
    (void)xQueueSend(queue,&item,0);
}
bool now_radio_init(void) {
    uint8_t pmk[16],lmk[16],auth[32];
    if(!parse_hex(CONFIG_ROBOT_ESPNOW_CONTROLLER_MAC,peer,6,true)||(peer[0]&1)||
       !parse_hex(CONFIG_ROBOT_ESPNOW_PMK,pmk,16,false)||
       !parse_hex(CONFIG_ROBOT_ESPNOW_LMK,lmk,16,false)||
       !parse_hex(CONFIG_ROBOT_ESPNOW_AUTH_KEY,auth,32,false)) {
        ESP_LOGW("espnow","Not provisioned: configure controller STA MAC and private PMK/LMK/AUTH_KEY; wireless control disabled");
        return false;
    }
    psa_key_attributes_t attr=PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_usage_flags(&attr,PSA_KEY_USAGE_SIGN_MESSAGE|PSA_KEY_USAGE_VERIFY_MESSAGE);
    psa_set_key_algorithm(&attr,AUTH_ALG);psa_set_key_type(&attr,PSA_KEY_TYPE_HMAC);
    psa_set_key_bits(&attr,256);
    psa_status_t crypto=psa_crypto_init();
    if(crypto==PSA_SUCCESS)crypto=psa_import_key(&attr,auth,sizeof(auth),&auth_key);
    psa_reset_key_attributes(&attr);memset(auth,0,sizeof(auth));
    if(crypto!=PSA_SUCCESS){ESP_LOGE("espnow","Authentication initialization failed; disabled");return false;}
    queue=xQueueCreate(8,sizeof(received_t));
    control_tx=xQueueCreate(4,sizeof(tx_item_t));wave_tx=xQueueCreate(1,sizeof(tx_item_t));
    if(!queue||!control_tx||!wave_tx)return false;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init_config_t cfg=WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_set_channel(CONFIG_ROBOT_ESPNOW_CHANNEL,WIFI_SECOND_CHAN_NONE));
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA,local));
    if(!memcmp(peer,local,6)) {ESP_LOGE("espnow","Controller MAC equals node MAC; disabled");return false;}
    ESP_ERROR_CHECK(esp_now_init());ESP_ERROR_CHECK(esp_now_set_pmk(pmk));
    esp_now_peer_info_t p={.channel=CONFIG_ROBOT_ESPNOW_CHANNEL,.ifidx=WIFI_IF_STA,.encrypt=true};
    memcpy(p.peer_addr,peer,6);memcpy(p.lmk,lmk,16);
    ESP_ERROR_CHECK(esp_now_add_peer(&p));
    ESP_ERROR_CHECK(esp_now_register_send_cb(send_cb));
    ESP_ERROR_CHECK(esp_now_register_recv_cb(receive_cb));ready=true;
    ESP_LOGI("espnow","Encrypted node %u STA=%02x:%02x:%02x:%02x:%02x:%02x channel=%d; STOP handshake required",
        (unsigned)CONFIG_ROBOT_ESPNOW_NODE_ID,local[0],local[1],local[2],local[3],local[4],local[5],CONFIG_ROBOT_ESPNOW_CHANNEL);
    return true;
}

static bool raw_receive(uint8_t out[64],uint32_t *received) {
    if(!ready)return false;
    received_t item;
    for(int i=0;i<8&&xQueueReceive(queue,&item,0)==pdTRUE;++i) {
        if(psa_mac_verify(auth_key,AUTH_ALG,item.data,64,item.data+64,16)==PSA_SUCCESS &&
           item.data[0]=='W' && item.data[1]=='N' && item.data[2]==1 &&
           now_crc32(item.data,60)==((uint32_t)item.data[60]|(uint32_t)item.data[61]<<8|
             (uint32_t)item.data[62]<<16|(uint32_t)item.data[63]<<24)) {
            memcpy(out,item.data,64);*received=item.at;return true;
        }
    }
    return false;
}
bool now_radio_receive_frame(uint8_t out[64],uint32_t *at) {return raw_receive(out,at);}
bool now_radio_receive(now_request_t *q,uint32_t *at) {
    uint8_t raw[64];
    for(int i=0;i<8&&raw_receive(raw,at);++i)if(now_decode(raw,64,q))return true;
    return false;
}
void now_radio_send(const uint8_t data[64]) {
    if(!ready)return;
    // STOP supersedes queued movement and waveform data before radio submission.
    if(data[3]==NOW_COMMAND && data[25]==ROBOT_STOP){xQueueReset(control_tx);xQueueReset(wave_tx);}
    tx_item_t item={.at=(uint32_t)(esp_timer_get_time()/1000)};memcpy(item.data,data,64);
    if(xQueueSend(control_tx,&item,0)!=pdTRUE) {
        tx_item_t old;(void)xQueueReceive(control_tx,&old,0);(void)xQueueSend(control_tx,&item,0);atomic_fetch_add(&control_drop,1);
    }
}
bool now_radio_send_wave(const uint8_t data[64]) {
    tx_item_t item={.at=(uint32_t)(esp_timer_get_time()/1000)};memcpy(item.data,data,64);
    return ready && uxQueueMessagesWaiting(control_tx)==0 && xQueueSend(wave_tx,&item,0)==pdTRUE;
}
void now_radio_service(void) {
    if(!ready || atomic_exchange(&sending,true))return;
    uint8_t packet[80];size_t tag_len=0;tx_item_t item;bool have=false;
    for(int i=0;i<5;i++){
        if(xQueueReceive(control_tx,&item,0)!=pdTRUE && xQueueReceive(wave_tx,&item,0)!=pdTRUE)break;
        bool stop=item.data[3]==NOW_COMMAND&&item.data[25]==ROBOT_STOP;
        if(!stop && (uint32_t)(esp_timer_get_time()/1000-item.at)>100){atomic_fetch_add(&expired,1);continue;}
        memcpy(packet,item.data,64);have=true;break;
    }
    if(!have){atomic_store(&sending,false);return;}
    if(psa_mac_compute(auth_key,AUTH_ALG,packet,64,packet+64,16,&tag_len)!=PSA_SUCCESS||tag_len!=16||
       esp_now_send(peer,packet,80)!=ESP_OK){atomic_fetch_add(&rejected,1);atomic_store(&sending,false);}
    else atomic_fetch_add(&submitted,1);
}
#else
void now_radio_stats(now_radio_stats_t *out){*out=(now_radio_stats_t){0};}
bool now_radio_init(void) {ESP_LOGI("espnow","Disabled");return false;}
bool now_radio_receive(now_request_t *q,uint32_t *at) {(void)q;(void)at;return false;}
bool now_radio_receive_frame(uint8_t out[64],uint32_t *at) {(void)out;(void)at;return false;}
void now_radio_send(const uint8_t d[64]) {(void)d;}
bool now_radio_send_wave(const uint8_t d[64]) {(void)d;return false;}
void now_radio_service(void) {}
#endif
