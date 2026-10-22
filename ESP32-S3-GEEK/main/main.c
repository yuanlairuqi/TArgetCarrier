// USB is a bounded transport. The PC owns WN/1 transactions and the STOP handshake.
#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/usb_serial_jtag.h"
#include "esp_timer.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "now_radio.h"
#include "usb_wire.h"
#include "panel.h"
static QueueHandle_t status_out,wave_out;
static portMUX_TYPE display_lock=portMUX_INITIALIZER_UNLOCKED;
static bool pc_online,output_ack,radio_online;
static int speed;
static uint32_t last_pc,last_radio;
static void screen_task(void *arg){
    (void)arg;panel_init();
    for(;;){
        portENTER_CRITICAL(&display_lock);
        bool pc=pc_online,ack=output_ack,radio=radio_online;int value=speed;
        portEXIT_CRITICAL(&display_lock);
        panel_draw(pc,ack,value,radio?"SLAVE STATUS RX":"SLAVE OFFLINE",0x102030,60);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
static uint32_t ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
static void queue_usb(uint8_t kind,const uint8_t body[64]){
    uint8_t packet[72];usb_pack(kind,body,packet);
    QueueHandle_t q=kind==4?wave_out:status_out;
    if(xQueueSend(q,packet,0)!=pdTRUE){
        uint8_t old[72];(void)xQueueReceive(q,old,0);(void)xQueueSend(q,packet,0);
    }
}
void app_main(void){
    esp_err_t err=nvs_flash_init();
    if(err==ESP_ERR_NVS_NO_FREE_PAGES||err==ESP_ERR_NVS_NEW_VERSION_FOUND){ESP_ERROR_CHECK(nvs_flash_erase());err=nvs_flash_init();}
    ESP_ERROR_CHECK(err);
    usb_serial_jtag_driver_config_t usb={.tx_buffer_size=256,.rx_buffer_size=256};
    ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&usb));
    status_out=xQueueCreate(8,72);wave_out=xQueueCreate(16,72);
    configASSERT(status_out&&wave_out);
    bool provisioned=now_radio_init();
    xTaskCreate(screen_task,"lcd",4096,NULL,1,NULL);
    usb_parser_t parser={0};uint8_t last_command[64]={0};bool armed_transport=false;
    uint8_t tx[72];size_t tx_at=72;uint32_t info_at=0;
    for(;;){
        uint32_t now=ms();uint8_t input[144],body[64],kind;
        int count=usb_serial_jtag_read_bytes(input,sizeof(input),0);
        for(int i=0;i<count;i++)if(usb_feed(&parser,input[i],body,&kind)&&kind==1){
            now_request_t q;
            if(!now_decode(body,64,&q)||q.node!=CONFIG_ROBOT_ESPNOW_NODE_ID)continue;
            if(q.kind==NOW_QUERY){armed_transport=true;memset(last_command,0,64);}
            if(!armed_transport)continue;
            if(q.kind==NOW_COMMAND)memcpy(last_command,body,64);
            last_pc=now;if(provisioned)now_radio_send(body);
        }
        bool pc=usb_serial_jtag_is_connected()&&(uint32_t)(now-last_pc)<600&&armed_transport;
        if(armed_transport&&!pc){
            // No replay/renewal on host loss. Best-effort STOP; slave's 800ms lease remains authoritative.
            if(last_command[3]==NOW_COMMAND){
                uint32_t seq=wire_get32(last_command+20)+1;
                wire_put32(last_command+20,seq);memset(last_command+24,0,16);last_command[24]=3;
                wire_put32(last_command+28,seq);wire_put32(last_command+60,now_crc32(last_command,60));
                now_radio_send(last_command);
            }
            armed_transport=false;parser.used=0;
        }
        for(int n=0;n<8;n++){
            uint32_t at;if(!now_radio_receive_frame(body,&at))break;
            if(now-at>100||wire_get32(body+4)!=CONFIG_ROBOT_ESPNOW_NODE_ID)continue;
            if(body[3]!=3&&body[3]!=4)continue;
            if(body[3]==3){
                last_radio=now;
                portENTER_CRITICAL(&display_lock);
                output_ack=pc && master_output_confirmed(body,last_command);speed=body[46];
                portEXIT_CRITICAL(&display_lock);
            }
            if(pc)queue_usb(body[3]==3?2:4,body);
        }
        now_radio_service();
        if(now-info_at>=1000){
            uint8_t info[64]={0};info[0]=provisioned;info[1]=CONFIG_ROBOT_ESPNOW_CHANNEL;
            wire_put32(info+4,CONFIG_ROBOT_ESPNOW_NODE_ID);wire_put32(info+8,now);esp_read_mac(info+12,ESP_MAC_WIFI_STA);
            now_radio_stats_t stats;now_radio_stats(&stats);
            wire_put32(info+20,stats.submitted);wire_put32(info+24,stats.link_ok);wire_put32(info+28,stats.link_fail);
            wire_put32(info+32,stats.rejected);wire_put32(info+36,stats.expired);wire_put32(info+40,stats.control_drop);
            queue_usb(5,info);info_at=now;
        }
        // At most one partially written frame, then status before waveform; never wait for USB.
        if(tx_at==72&&(xQueueReceive(status_out,tx,0)==pdTRUE||xQueueReceive(wave_out,tx,0)==pdTRUE))tx_at=0;
        if(tx_at<72){int sent=usb_serial_jtag_write_bytes(tx+tx_at,72-tx_at,0);if(sent>0)tx_at+=(size_t)sent;}
        if(!usb_serial_jtag_is_connected()){tx_at=72;xQueueReset(status_out);xQueueReset(wave_out);}
        portENTER_CRITICAL(&display_lock);pc_online=pc;radio_online=last_radio!=0&&now-last_radio<1000;if(!radio_online||!pc)output_ack=false;portEXIT_CRITICAL(&display_lock);
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}
