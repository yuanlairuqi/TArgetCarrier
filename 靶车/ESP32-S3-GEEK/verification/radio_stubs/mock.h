#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_ROBOT_ESPNOW_ENABLED 1
#define CONFIG_ROBOT_ESPNOW_CONTROLLER_MAC ""
#define CONFIG_ROBOT_ESPNOW_PMK ""
#define CONFIG_ROBOT_ESPNOW_LMK ""
#define CONFIG_ROBOT_ESPNOW_AUTH_KEY ""
#define CONFIG_ROBOT_ESPNOW_CHANNEL 1
#define CONFIG_ROBOT_ESPNOW_NODE_ID 1
#define pdTRUE 1
#define ESP_OK 0
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGD(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGE(...) ((void)0)
#define ESP_ERROR_CHECK(x) ((void)(x))
#define WIFI_INIT_CONFIG_DEFAULT() ((wifi_init_config_t){0})
#define WIFI_STORAGE_RAM 0
#define WIFI_MODE_STA 0
#define WIFI_PS_NONE 0
#define WIFI_SECOND_CHAN_NONE 0
#define WIFI_IF_STA 0
typedef int esp_err_t;
typedef struct {int unused;} wifi_init_config_t;
typedef struct {uint8_t *src_addr,*des_addr;} esp_now_recv_info_t;
typedef struct {int unused;} esp_now_send_info_t;
typedef int esp_now_send_status_t;
#define ESP_NOW_SEND_SUCCESS 0
typedef struct {uint8_t channel,ifidx;bool encrypt;uint8_t peer_addr[6],lmk[16];} esp_now_peer_info_t;
typedef struct {unsigned count,capacity,size,head;uint8_t *data;} *QueueHandle_t;
static QueueHandle_t xQueueCreate(unsigned count,unsigned size){QueueHandle_t q=calloc(1,sizeof(*q));q->capacity=count;q->size=size;q->data=calloc(count,size);return q;}
static int xQueueSend(QueueHandle_t q,const void *p,int wait){(void)wait;if(q->count==q->capacity)return 0;memcpy(q->data+((q->head+q->count)%q->capacity)*q->size,p,q->size);q->count++;return 1;}
static int xQueueReceive(QueueHandle_t q,void *p,int wait){(void)wait;if(!q->count)return 0;memcpy(p,q->data+q->head*q->size,q->size);q->head=(q->head+1)%q->capacity;q->count--;return 1;}
static unsigned uxQueueMessagesWaiting(QueueHandle_t q){return q->count;}
static void xQueueReset(QueueHandle_t q){q->count=q->head=0;}
static int64_t mock_time;
static int64_t esp_timer_get_time(void){return mock_time;}
static uint8_t mock_sent[80];static unsigned mock_sends;static int mock_send_error;
static int esp_now_send(const uint8_t *peer,const uint8_t *p,unsigned size){(void)peer;memcpy(mock_sent,p,size);mock_sends++;return mock_send_error;}
#define esp_netif_init() 0
#define esp_event_loop_create_default() 0
#define esp_wifi_init(x) ((void)(x),0)
#define esp_wifi_set_storage(x) 0
#define esp_wifi_set_mode(x) 0
#define esp_wifi_start() 0
#define esp_wifi_set_ps(x) 0
#define esp_wifi_set_channel(x,y) 0
#define esp_wifi_get_mac(x,y) ((void)(y),0)
#define esp_now_init() 0
#define esp_now_set_pmk(x) ((void)(x),0)
#define esp_now_add_peer(x) ((void)(x),0)
#define esp_now_register_send_cb(x) ((void)(x),0)
#define esp_now_register_recv_cb(x) ((void)(x),0)
typedef int psa_key_id_t;
typedef int psa_status_t;
typedef int psa_key_attributes_t;
#define PSA_KEY_ATTRIBUTES_INIT 0
#define PSA_KEY_USAGE_SIGN_MESSAGE 1
#define PSA_KEY_USAGE_VERIFY_MESSAGE 2
#define PSA_KEY_TYPE_HMAC 0
#define PSA_ALG_SHA_256 0
#define PSA_ALG_HMAC(x) 0
#define PSA_ALG_TRUNCATED_MAC(x,y) 0
#define PSA_SUCCESS 0
#define psa_set_key_usage_flags(a,b) ((void)(a))
#define psa_set_key_algorithm(a,b) ((void)(a))
#define psa_set_key_type(a,b) ((void)(a))
#define psa_set_key_bits(a,b) ((void)(a))
#define psa_crypto_init() 0
#define psa_import_key(a,b,c,d) ((void)(a),(void)(b),(void)(c),(void)(d),0)
#define psa_reset_key_attributes(a) ((void)(a))
#define psa_mac_verify(a,b,c,d,e,f) ((void)(a),(void)(c),(void)(d),(void)(e),(void)(f),0)
static int psa_mac_compute(int key,int alg,const uint8_t *p,unsigned len,uint8_t *tag,unsigned size,size_t *out){(void)key;(void)alg;(void)p;(void)len;memset(tag,0,size);*out=size;return 0;}
