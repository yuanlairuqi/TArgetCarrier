#include "capture_core/source.h"
/* PC BLE -> existing 17-byte UART -> STM32/CAN. No ESP-NOW owner in this build.
 * PC generates trajectories; this bridge independently enforces source/TTL,
 * fresh feedback, STOP priority and failsafe. No command is replayed on reconnect.
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "wl_esp.h"
#include "tc_board.h"
#if TC_BACKEND != TC_I2C
#error "Target I2C firmware requires TC_I2C on both receivers"
#endif
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ble_guard.h"
#include "now_radio.h"
#include "now_session.h"
#include "oled.h"

/* Different service from x axis 6e400001: never select a device by name only. */
static const ble_uuid128_t svc_uuid=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x01,0x00,0x41,0x6e);
static const ble_uuid128_t cmd_uuid=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x02,0x00,0x41,0x6e);
static const ble_uuid128_t status_uuid=BLE_UUID128_INIT(0x9e,0xca,0xdc,0x24,0x0e,0xe5,0xa9,0xe0,0x93,0xf3,0xa3,0xb5,0x03,0x00,0x41,0x6e);
typedef struct {uint8_t data[17];uint32_t at,epoch;} item_t;
static QueueHandle_t safety_q,cmd_q,gain_q,param_q;
static SemaphoreHandle_t state_lock;
static volatile uint16_t conn=BLE_HS_CONN_HANDLE_NONE;
static volatile bool subscribed;
static volatile uint32_t epoch=1;
static uint8_t addr_type,inject_seq;
static uint16_t status_handle;
static uint32_t boot_id,stm_at,motor_at,last_health,last_status_up,last_motor_up,last_inject;
static uint32_t drops,uart_errors;
static bool have_status,have_motor,guard_connected;
static bool radio_available;static tn_session_t radio;static uint32_t radio_generation;
static bool radio_stop(const uint8_t *f){return tb_frame_valid(f,17)&&f[2]==PROTO_MSG_CTRL&&(!(f[4]&1)||(f[4]&12));}
static tb_guard_t guard;
static status_frame_t latest_status;
static lc_rx_t rx;
static uint32_t now_ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static bool fresh(void){return have_status&&(uint32_t)(now_ms()-stm_at)<=300U;}
static void put32(uint8_t *p,uint32_t v){for(int i=0;i<4;i++)p[i]=(uint8_t)(v>>(8*i));}
static void put16(uint8_t *p,uint32_t v){if(v>65535)v=65535;p[0]=(uint8_t)v;p[1]=(uint8_t)(v>>8);}
static void clear_queues(void){xQueueReset(safety_q);xQueueReset(cmd_q);xQueueReset(gain_q);xQueueReset(param_q);}
static void uart_send(const uint8_t f[17]){
    /* One UART sequence owner across PC reconnects and failsafe injections.
     * Format/flags/ARM token remain the existing STM32 protocol. */
    uint8_t out[17];memcpy(out,f,17);out[3]=inject_seq++;
    out[15]=(uint8_t)(proto_crc16(out+1,14)&0xff);
    if(!wl_esp_send(out,17,radio_stop(out)))++uart_errors;
}
static void inject_stop(uint32_t now){
    ctrl_target_t t={0};uint8_t f[17];
    proto_encode_ctrl(f,&t,CTRL_FLAG_FAILSAFE|CTRL_FLAG_ESTOP,MOTOR_MODE_MIT,0);
    uart_send(f);last_inject=now;
}
static void radio_publish(const void *data,size_t size,uint8_t result,uint8_t request_kind,uint32_t session,uint32_t seq){
    uint8_t out[64]={'W','N',1,3};put32(out+4,2);put32(out+8,boot_id);put32(out+12,epoch);
    put32(out+16,session);put32(out+20,seq);out[24]=result;out[25]=radio.active?2:conn!=BLE_HS_CONN_HANDLE_NONE?1:0;out[26]=request_kind;out[27]=(uint8_t)size;
    if(size<=20&&data){memcpy(out+40,data,size);}
    put32(out+60,now_crc32(out,60));now_radio_send(out);
}
static void notify(const void *data,size_t size){
    if(radio.active)radio_publish(data,size,NOW_OK,0,radio.session,radio.seq);
    uint16_t c=conn;if(!subscribed||c==BLE_HS_CONN_HANDLE_NONE)return;
    struct os_mbuf *om=ble_hs_mbuf_from_flat(data,size);
    if(om){int rc=ble_gatts_notify_custom(c,status_handle,om);if(rc&&rc!=BLE_HS_ENOTCONN)++drops;}
}
static void health(uint8_t out[20]){
    memset(out,0,20);out[0]='T';out[1]='B';out[2]=1;
    bool f=fresh();out[3]=(guard.connected?1:0)|(f?2:0)|(guard.failsafe?4:0)|(guard.disarm_seen?8:0)|(guard.active?16:0);
    put32(out+4,boot_id);put32(out+8,epoch);
    put16(out+12,have_status?(uint32_t)(now_ms()-stm_at):65535);
    put16(out+14,have_motor?(uint32_t)(now_ms()-motor_at):65535);
    out[16]=guard.seq.last;out[17]=(uint8_t)guard.rejected;put16(out+18,uart_errors);
}
static int access_status(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctxt,void *arg){
    (void)c;(void)a;(void)arg;uint8_t f[20];
    xSemaphoreTake(state_lock,portMAX_DELAY);health(f);xSemaphoreGive(state_lock);
    return os_mbuf_append(ctxt->om,f,sizeof(f))?BLE_ATT_ERR_INSUFFICIENT_RES:0;
}
static int access_cmd(uint16_t c,uint16_t a,struct ble_gatt_access_ctxt *ctxt,void *arg){
    (void)a;(void)arg;item_t item={0};uint16_t len=0;
    if(c!=conn||OS_MBUF_PKTLEN(ctxt->om)!=17||ble_hs_mbuf_to_flat(ctxt->om,item.data,17,&len)||
       !tb_frame_valid(item.data,len))return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    item.at=now_ms();
    xSemaphoreTake(state_lock,portMAX_DELAY);
    if(radio.active){
        bool stop=radio_stop(item.data);
        if(stop){radio.active=false;radio.session=0;++epoch;clear_queues();tb_guard_connection(&guard,conn!=BLE_HS_CONN_HANDLE_NONE);inject_stop(item.at);}
        xSemaphoreGive(state_lock);return stop?0:BLE_ATT_ERR_UNLIKELY;
    }
    item.epoch=epoch;
    if(item.data[2]==PROTO_MSG_CTRL){
        ctrl_payload_t p;proto_decode_ctrl(item.data,17,&p,NULL);
        if(!(p.flags&CTRL_FLAG_ENABLE)||(p.flags&(CTRL_FLAG_ESTOP|CTRL_FLAG_FAILSAFE))){
            xQueueReset(cmd_q);xQueueOverwrite(safety_q,&item);
        }else xQueueOverwrite(cmd_q,&item);
    }else if(item.data[2]==PROTO_MSG_GAINS)xQueueOverwrite(gain_q,&item);
    else if(xQueueSend(param_q,&item,0)!=pdTRUE)++drops;
    xSemaphoreGive(state_lock);
    return 0; /* transport accepted, execution is confirmed only by STM32 data */
}
static const struct ble_gatt_svc_def services[]={
    {.type=BLE_GATT_SVC_TYPE_PRIMARY,.uuid=&svc_uuid.u,.characteristics=(struct ble_gatt_chr_def[]){
        {.uuid=&cmd_uuid.u,.access_cb=access_cmd,.flags=BLE_GATT_CHR_F_WRITE},
        {.uuid=&status_uuid.u,.access_cb=access_status,.flags=BLE_GATT_CHR_F_READ|BLE_GATT_CHR_F_NOTIFY,.val_handle=&status_handle},{0}}},{0}};
static int gap_event(struct ble_gap_event *,void *);
static void advertise(void){
    struct ble_hs_adv_fields f={0},response={0};f.flags=BLE_HS_ADV_F_DISC_GEN|BLE_HS_ADV_F_BREDR_UNSUP;
    f.uuids128=(ble_uuid128_t *)&svc_uuid;f.num_uuids128=1;f.uuids128_is_complete=1;
    const char *name=ble_svc_gap_device_name();response.name=(uint8_t *)name;response.name_len=strlen(name);response.name_is_complete=1;
    int rc=ble_gap_adv_set_fields(&f);if(!rc)rc=ble_gap_adv_rsp_set_fields(&response);
    struct ble_gap_adv_params p={.conn_mode=BLE_GAP_CONN_MODE_UND,.disc_mode=BLE_GAP_DISC_MODE_GEN};
    if(!rc)rc=ble_gap_adv_start(addr_type,NULL,BLE_HS_FOREVER,&p,gap_event,NULL);
    ESP_LOGI("target_ble","advertise rc=%d",rc);
}
static int gap_event(struct ble_gap_event *event,void *arg){
    (void)arg;
    switch(event->type){
    case BLE_GAP_EVENT_CONNECT:
        if(!event->connect.status){xSemaphoreTake(state_lock,portMAX_DELAY);conn=event->connect.conn_handle;subscribed=false;if(!radio.active){__atomic_add_fetch(&epoch,1,__ATOMIC_SEQ_CST);clear_queues();}xSemaphoreGive(state_lock);}
        else advertise();
        ESP_LOGI("target_ble","connect status=%d",event->connect.status);break;
    case BLE_GAP_EVENT_DISCONNECT:
        xSemaphoreTake(state_lock,portMAX_DELAY);conn=BLE_HS_CONN_HANDLE_NONE;subscribed=false;if(!radio.active){__atomic_add_fetch(&epoch,1,__ATOMIC_SEQ_CST);clear_queues();}
        xSemaphoreGive(state_lock);ESP_LOGI("target_ble","disconnect reason=%d",event->disconnect.reason);advertise();break;
    case BLE_GAP_EVENT_SUBSCRIBE:if(event->subscribe.attr_handle==status_handle)subscribed=event->subscribe.cur_notify;break;
    case BLE_GAP_EVENT_ADV_COMPLETE:advertise();break;
    default:break;
    }return 0;
}
static void sync_host(void){assert(!ble_hs_util_ensure_addr(0));assert(!ble_hs_id_infer_auto(0,&addr_type));advertise();}
static void reset_host(int reason){xSemaphoreTake(state_lock,portMAX_DELAY);conn=BLE_HS_CONN_HANDLE_NONE;subscribed=false;if(!radio.active){__atomic_add_fetch(&epoch,1,__ATOMIC_SEQ_CST);clear_queues();}xSemaphoreGive(state_lock);ESP_LOGE("target_ble","host reset=%d",reason);}
static void host_task(void *arg){(void)arg;nimble_port_run();nimble_port_freertos_deinit();}
static cap_source recording_source;
static void record_put(uint8_t*p,uint32_t x){for(unsigned i=0;i<4;i++)p[i]=(uint8_t)(x>>(8*i));}
static bool record_send(const uint8_t fragment[20],void *ctx){
 (void)ctx;uint8_t out[64]={'W','N',1,4};record_put(out+4,2);record_put(out+8,boot_id);record_put(out+12,epoch);
 record_put(out+16,radio.session);out[32]=3;memcpy(out+40,fragment,20);record_put(out+60,now_crc32(out,60));return now_radio_send_wave(out);
}
static void worker(void *arg){
    (void)arg;uint32_t seen_epoch=0,last_byte=0;uint8_t last_status_seq=0,last_motor_seq=0;
    for(;;){
        uint32_t now=now_ms();xSemaphoreTake(state_lock,portMAX_DELAY);
        if(seen_epoch!=epoch){seen_epoch=epoch;guard_connected=radio.active||conn!=BLE_HS_CONN_HANDLE_NONE;tb_guard_connection(&guard,guard_connected);inject_stop(now);}
        now_radio_stats_t rs;now_radio_stats(&rs);
        if(radio.active&&((uint32_t)(now-radio.last)>=600U||radio_generation!=rs.recoveries)){
            radio.active=false;radio.session=0;++epoch;clear_queues();tb_guard_connection(&guard,conn!=BLE_HS_CONN_HANDLE_NONE);inject_stop(now);
        }
        radio_generation=rs.recoveries;
        for(unsigned batch=0;batch<4;batch++){
            now_request_t q;uint32_t at;if(!now_radio_receive(&q,&at))break;if(q.node!=2)continue;
            uint8_t result=NOW_OK;
            if(q.kind!=NOW_QUERY){
                bool was_active=radio.active;
                result=tn_accept(&radio,&q,boot_id,epoch,at,now);
                if(result==NOW_OK){
                    if(!was_active){clear_queues();tb_guard_connection(&guard,true);}
                    if(tb_guard_accept(&guard,q.command,now,fresh()))uart_send(q.command);
                    else{if(radio_stop(q.command))inject_stop(now);result=NOW_BAD_COMMAND;}
                }else if(result==NOW_REPLAY&&radio_stop(q.command))inject_stop(now);
            }
            uint8_t h[20];health(h);radio_publish(h,20,result,q.kind,q.kind==NOW_QUERY?radio.session:q.session,q.kind==NOW_QUERY?radio.seq:q.seq);
        }
        uint8_t bytes[256],frame[17];int n=wl_esp_read(bytes,sizeof(bytes));
        if(rx.len&&(uint32_t)(now-last_byte)>20)lc_rx_overflow(&rx);
        for(int i=0;i<n;i++){
            last_byte=now;lc_rx_kind_t kind=lc_rx_feed(&rx,bytes[i],now,frame);
            if(kind==LC_RX_STATUS){status_frame_t st;
                if(proto_decode_status(frame,17,&st)&&(!have_status||lc_seq_cmp8(st.seq,last_status_seq)>0)){
                    last_status_seq=st.seq;latest_status=st;have_status=true;stm_at=now;
                    static uint32_t record_at,last_event=UINT32_MAX;
                    uint64_t record_us=(uint64_t)esp_timer_get_time();
                    if(now-record_at>=CAP_SNAPSHOT_PERIOD_MS){record_at=now;uint32_t bits;float yaw=st.yaw_deg;memcpy(&bits,&yaw,4);
                        cap_source_feed(&recording_source,CAP_SAMPLE,1,CAP_F32,3,record_us,bits);}
                    uint32_t code=((uint32_t)st.state<<16)|st.err_code;
                    if(code!=last_event){last_event=code;cap_source_feed(&recording_source,CAP_EVENT,100,CAP_U32,0,record_us,code);}

                    if((uint32_t)(now-last_status_up)>=100){notify(frame,17);last_status_up=now;}
                }
            }else if(kind==LC_RX_MOTOR){float p,v,t;uint16_t fail;
                if(proto_decode_motor(frame,17,&p,&v,&t,&fail)&&(!have_motor||lc_seq_cmp8(frame[3],last_motor_seq)>0)){
                    last_motor_seq=frame[3];have_motor=true;motor_at=now;
                    if((uint32_t)(now-last_motor_up)>=100){notify(frame,17);last_motor_up=now;}
                }
            }else if(kind==LC_RX_PARAM_REPLY)notify(frame,17);
        }
        if(tb_guard_tick(&guard,now,fresh())){clear_queues();inject_stop(now);if(radio.active){radio.active=false;radio.session=0;++epoch;}}
        QueueHandle_t queues[]={safety_q,cmd_q,gain_q,param_q};
        for(unsigned q=0;q<4;q++){item_t item;
            if(xQueueReceive(queues[q],&item,0)==pdTRUE){
                if(radio.active||item.epoch!=epoch||(uint32_t)(now-item.at)>100){++drops;continue;}
                if(tb_guard_accept(&guard,item.data,now,fresh()))uart_send(item.data);
            }
        }
        if(guard.failsafe&&(uint32_t)(now-last_inject)>=50)inject_stop(now);
        if((uint32_t)(now-last_health)>=200){uint8_t f[20];health(f);notify(f,20);last_health=now;}
        xSemaphoreGive(state_lock);cap_source_poll(&recording_source,radio.active,(uint64_t)esp_timer_get_time(),record_send,NULL);now_radio_service();vTaskDelay(pdMS_TO_TICKS(2)>0?pdMS_TO_TICKS(2):1);
    }
}
static void scl(uint8_t v){gpio_set_level(GPIO_NUM_9,v);}
static void sda(uint8_t v){gpio_set_level(GPIO_NUM_8,v);}
static uint8_t read_sda(void){return (uint8_t)gpio_get_level(GPIO_NUM_8);}
static void delay_us(uint32_t us){esp_rom_delay_us(us);}
static void display_task(void *arg){
    (void)arg;gpio_config_t cfg={.pin_bit_mask=(1ULL<<8)|(1ULL<<9),.mode=GPIO_MODE_INPUT_OUTPUT_OD,.pull_up_en=GPIO_PULLUP_ENABLE};
    if(gpio_config(&cfg)!=ESP_OK){vTaskDelete(NULL);return;}
    scl(1);sda(1);static const oled_hal_t hal={scl,sda,read_sda,delay_us};
    if(!oled_init(&hal,OLED_CTRL_SSD1306,OLED_I2C_ADDR_3C)&&!oled_init(&hal,OLED_CTRL_SSD1306,OLED_I2C_ADDR_3D)){vTaskDelete(NULL);return;}
    for(;;){char line[48];bool linked,stm;status_frame_t st;
        xSemaphoreTake(state_lock,portMAX_DELAY);linked=guard.connected;stm=fresh();st=latest_status;xSemaphoreGive(state_lock);
        oled_clear();oled_text(0,0,"TC-TARGET BLE/NOW",1);
        snprintf(line,sizeof(line),"BLE %s STM %s",linked?"ON":"OFF",stm?"OK":"STALE");oled_text(0,12,line,1);
        snprintf(line,sizeof(line),"YAW %.1f",st.yaw_deg);oled_text(0,24,line,1);
        snprintf(line,sizeof(line),"STATE %u ERR %u",st.state,st.err_code);oled_text(0,36,line,1);
        oled_text(0,48,"I2C STM32 link",1);oled_flush();vTaskDelay(pdMS_TO_TICKS(500));
    }
}
void app_main(void){
    esp_err_t rc=nvs_flash_init();if(rc==ESP_ERR_NVS_NO_FREE_PAGES||rc==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());rc=nvs_flash_init();}ESP_ERROR_CHECK(rc);
    state_lock=xSemaphoreCreateMutex();safety_q=xQueueCreate(1,sizeof(item_t));cmd_q=xQueueCreate(1,sizeof(item_t));gain_q=xQueueCreate(1,sizeof(item_t));param_q=xQueueCreate(4,sizeof(item_t));assert(state_lock&&safety_q&&cmd_q&&gain_q&&param_q);
    tb_guard_init(&guard);lc_rx_init(&rx);boot_id=esp_random();if(!boot_id)boot_id=1;cap_source_init(&recording_source,2,boot_id);
    if(!wl_esp_init())return; /* I2C owns STM32 communication; no UART driver installed. */
    radio_available=now_radio_init();(void)radio_available;
    ESP_ERROR_CHECK(nimble_port_init());ble_hs_cfg.sync_cb=sync_host;ble_hs_cfg.reset_cb=reset_host;
    ble_svc_gap_init();ble_svc_gatt_init();ESP_ERROR_CHECK(ble_gatts_count_cfg(services));ESP_ERROR_CHECK(ble_gatts_add_svcs(services));ESP_ERROR_CHECK(ble_svc_gap_device_name_set("TC-Target"));
    nimble_port_freertos_init(host_task);assert(xTaskCreate(worker,"target_i2c",6144,NULL,8,NULL)==pdPASS);
    assert(xTaskCreate(display_task,"target_oled",3072,NULL,1,NULL)==pdPASS);
    ESP_LOGI("target_ble","Ready: TC-Target, I2C STM32 transport, optional GEEK session; node=2");
}
