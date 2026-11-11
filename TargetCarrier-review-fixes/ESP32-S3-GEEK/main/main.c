// USB/ESP-NOW relay. Fresh STOP handshakes unlock every new PC session.
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "now_radio.h"
#include "usb_wire.h"
#include "master_guard.h"
#include "panel.h"
typedef struct {uint8_t bytes[72];uint32_t at;} usb_item_t;
static QueueHandle_t status_out,wave_out;
static portMUX_TYPE display_lock=portMUX_INITIALIZER_UNLOCKED;
static bool pc_online,output_ack,radio_online,radio_provisioned,stm_online;
static int speed;
static uint32_t usb_drop;
static uint32_t ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static void screen_task(void *arg){
    (void)arg;panel_init();
    for(;;){
        portENTER_CRITICAL(&display_lock);
        bool pc=pc_online,ack=output_ack,radio=radio_online,stm=stm_online;int value=speed;
        portEXIT_CRITICAL(&display_lock);
        panel_draw(pc,ack,value,!radio_provisioned?"PAIRING REQUIRED":!radio?"SLAVE OFFLINE":!stm?"STM OFFLINE":"SLAVE ONLINE",0x102030,60);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
static void queue_usb(uint8_t kind,const uint8_t body[64]){
    usb_item_t item={.at=ms()};usb_pack(kind,body,item.bytes);
    QueueHandle_t q=kind==4?wave_out:status_out;
    if(xQueueSend(q,&item,0)!=pdTRUE){usb_item_t old;(void)xQueueReceive(q,&old,0);(void)xQueueSend(q,&item,0);++usb_drop;}
}
static void lose_transport(master_guard_t *g,uint8_t last[64]){
    now_radio_flush();
    if(g->active&&last[3]==NOW_COMMAND){
        uint32_t seq=wire_get32(last+20)+1;
        wire_put32(last+20,seq);memset(last+24,0,16);last[24]=3;
        wire_put32(last+28,seq);wire_put32(last+60,now_crc32(last,60));now_radio_send(last);
    }
    master_guard_loss(g);memset(last,0,64);xQueueReset(status_out);xQueueReset(wave_out);
}
void app_main(void){
    esp_err_t err=nvs_flash_init();
    if(err==ESP_ERR_NVS_NO_FREE_PAGES||err==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());err=nvs_flash_init();}
    ESP_ERROR_CHECK(err);
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=512,.rx_buffer_size=512};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    status_out=xQueueCreate(8,sizeof(usb_item_t));wave_out=xQueueCreate(16,sizeof(usb_item_t));configASSERT(status_out&&wave_out);
    bool provisioned=now_radio_init();radio_provisioned=provisioned;
    configASSERT(xTaskCreate(screen_task,"lcd",4096,NULL,1,NULL)==pdPASS);
    usb_parser_t parser={0};master_guard_t guard={0};uint8_t last_command[64]={0};
    usb_item_t tx={0};size_t tx_at=72;uint32_t info_at=0,tx_started=0,last_loop=0,max_gap=0,generation=0,diag_at=0;
    uint32_t boot=esp_random();if(!boot)boot=1;
    for(;;){
        uint32_t now=ms();if(last_loop&&now-last_loop>max_gap)max_gap=now-last_loop;last_loop=now;
        now_radio_stats_t stats;now_radio_stats(&stats);
        if(generation!=stats.recoveries){generation=stats.recoveries;lose_transport(&guard,last_command);parser.used=0;}
        bool usb_connected=usb_serial_jtag_is_connected();
        // Loss detection precedes draining buffered commands; expired sessions never renew themselves.
        if(master_guard_expired(&guard,usb_connected,now)){
            lose_transport(&guard,last_command);parser.used=0;
            uint8_t discard[512];(void)usb_serial_jtag_read_bytes(discard,sizeof(discard),0);
        }
        uint8_t input[288],body[64],kind;
        int count=usb_serial_jtag_read_bytes(input,sizeof(input),0);
        for(int i=0;i<count;i++)if(usb_feed(&parser,input[i],body,&kind)&&kind==1&&usb_connected){
            now_request_t q;
            if(!now_decode(body,64,&q)||q.node!=CONFIG_ROBOT_ESPNOW_NODE_ID)continue;
            if(q.kind==NOW_QUERY)master_guard_query(&guard,now);
            else if(!master_guard_command(&guard,&q,now))continue;
            if(q.kind==NOW_COMMAND)memcpy(last_command,body,64);
            if(provisioned)now_radio_send(body);
        }
        for(int n=0;n<16;n++){
            uint32_t at;if(!now_radio_receive_frame(body,&at))break;
            if(now-at>100||wire_get32(body+4)!=CONFIG_ROBOT_ESPNOW_NODE_ID)continue;
            if(body[3]!=3&&body[3]!=4)continue;
            if(body[3]==3){
                if(!master_guard_status(&guard,wire_get32(body+8),wire_get32(body+12),now))continue;
                bool applied=master_output_confirmed(body,last_command);
                if(guard.active&&last_command[25]==0&&applied)guard.unlocked=true;
                portENTER_CRITICAL(&display_lock);
                output_ack=guard.active&&applied;speed=body[46];stm_online=(body[42]&8)!=0;
                portEXIT_CRITICAL(&display_lock);
            }else if(!guard.active||wire_get32(body+16)!=guard.session||wire_get32(body+8)!=guard.boot||wire_get32(body+12)!=guard.epoch)continue;
            if(usb_connected)queue_usb(body[3]==3?2:4,body);
        }
        now_radio_service();
        if(now-info_at>=1000){
            uint8_t info[64]={0};info[0]=provisioned;info[1]=CONFIG_ROBOT_ESPNOW_CHANNEL;
            wire_put32(info+4,CONFIG_ROBOT_ESPNOW_NODE_ID);wire_put32(info+8,now);esp_read_mac(info+12,ESP_MAC_WIFI_STA);
            now_radio_stats(&stats);
            wire_put32(info+20,stats.submitted);wire_put32(info+24,stats.link_ok);wire_put32(info+28,stats.link_fail);
            wire_put32(info+32,stats.rejected);wire_put32(info+36,stats.expired);wire_put32(info+40,stats.control_drop);
            wire_put32(info+44,boot);info[48]=(uint8_t)esp_reset_reason();uint8_t actual_channel=0;wifi_second_chan_t second;
            if(provisioned&&esp_wifi_get_channel(&actual_channel,&second)==ESP_OK)info[49]=actual_channel;
            info[50]=(uint8_t)stats.rx_high;info[51]=(uint8_t)stats.tx_high;
            info[52]=(uint8_t)stats.rx_drop;info[53]=(uint8_t)(stats.rx_drop>>8);
            info[54]=(uint8_t)stats.callback_timeout;info[55]=(uint8_t)(stats.callback_timeout>>8);
            info[56]=(uint8_t)stats.recoveries;info[57]=(uint8_t)(stats.recoveries>>8);
            info[58]=(uint8_t)max_gap;info[59]=(uint8_t)(max_gap>>8);
            info[60]=(uint8_t)usb_drop;info[61]=(uint8_t)(usb_drop>>8);info[62]=(uint8_t)stats.auth_fail;info[63]=1;
            queue_usb(5,info);info_at=now;
        }
        if(tx_at==72){
            for(int n=0;n<24;n++){
                if(xQueueReceive(status_out,&tx,0)!=pdTRUE&&xQueueReceive(wave_out,&tx,0)!=pdTRUE)break;
                if(now-tx.at>100){++usb_drop;continue;}
                tx_at=0;tx_started=now;break;
            }
        }
        if(tx_at<72){
            int sent=usb_serial_jtag_write_bytes(tx.bytes+tx_at,72-tx_at,0);if(sent>0)tx_at+=(size_t)sent;
            if(now-tx_started>=250){++usb_drop;tx_at=72;lose_transport(&guard,last_command);}
        }
        if(!usb_connected){tx_at=72;xQueueReset(status_out);xQueueReset(wave_out);}
        portENTER_CRITICAL(&display_lock);
        pc_online=usb_connected&&guard.queried&&now-guard.last_pc<600;
        radio_online=guard.challenge&&now-guard.last_radio<600;
        if(!radio_online||!pc_online)output_ack=false;
        portEXIT_CRITICAL(&display_lock);
        if(now-diag_at>=5000){diag_at=now;ESP_LOGI("link_diag","rx_drop=%lu cb_timeout=%lu generation=%lu usb_drop=%lu loop_max_ms=%lu auth_fail=%lu",
            (unsigned long)stats.rx_drop,(unsigned long)stats.callback_timeout,(unsigned long)stats.recoveries,(unsigned long)usb_drop,(unsigned long)max_gap,(unsigned long)stats.auth_fail);}
        // pdMS_TO_TICKS(5) is zero at 100 Hz. Block >=1 tick so idle/WDT and the LCD can run.
        vTaskDelay(pdMS_TO_TICKS(5)>0?pdMS_TO_TICKS(5):1);
    }
}
