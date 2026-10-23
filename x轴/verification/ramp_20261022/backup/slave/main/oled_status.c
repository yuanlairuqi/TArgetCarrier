#include "oled_status.h"
#include "sdkconfig.h"
#ifdef CONFIG_ROBOT_OLED_ENABLED
#include "oled_text.h"
#if CONFIG_ROBOT_OLED_HEIGHT != 32 && CONFIG_ROBOT_OLED_HEIGHT != 64
#error OLED height must be 32 or 64
#endif
#if CONFIG_ROBOT_OLED_ADDRESS != 0 && CONFIG_ROBOT_OLED_ADDRESS != 60 && CONFIG_ROBOT_OLED_ADDRESS != 61
#error OLED address must be 0, 60 or 61
#endif
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdio.h>
static portMUX_TYPE display_lock=portMUX_INITIALIZER_UNLOCKED;
static oled_status_t latest;
static const char *TAG="oled";
static i2c_master_bus_handle_t bus;
static i2c_master_dev_handle_t device;
static bool online;
static uint8_t active_address;
void oled_status_publish(const oled_status_t *s)
{
    portENTER_CRITICAL(&display_lock);latest=*s;portEXIT_CRITICAL(&display_lock);
}
static bool write_bus(void *ctx,uint8_t address,const uint8_t *bytes,size_t count)
{
    (void)ctx;
    return device && address==active_address && i2c_master_transmit(device,bytes,count,20)==ESP_OK;
}
static bool pins_valid(void)
{
    int scl=CONFIG_ROBOT_OLED_SCL_GPIO,sda=CONFIG_ROBOT_OLED_SDA_GPIO;
    if(scl==sda || !GPIO_IS_VALID_OUTPUT_GPIO(scl) || !GPIO_IS_VALID_OUTPUT_GPIO(sda)) return false;
    const int reserved[]={19,20, /* native USB */
#ifdef CONFIG_ROBOT_UART_ENABLED
        CONFIG_ROBOT_UART_TX_GPIO,CONFIG_ROBOT_UART_RX_GPIO,
#endif
#ifdef CONFIG_ROBOT_PULSE_CAPTURE_ENABLED
        CONFIG_ROBOT_PULSE_PUL_GPIO,CONFIG_ROBOT_PULSE_DIR_GPIO,
#endif
        -1};
    for(unsigned i=0;i<sizeof(reserved)/sizeof(reserved[0]);i++)
        if(scl==reserved[i] || sda==reserved[i]) return false;
    return true;
}
static bool connect_display(oled_text_t *d)
{
    if(device){i2c_master_bus_rm_device(device);device=NULL;}
    const uint8_t candidates[]={CONFIG_ROBOT_OLED_ADDRESS?CONFIG_ROBOT_OLED_ADDRESS:0x3c,0x3d};
    for(unsigned i=0;i<(CONFIG_ROBOT_OLED_ADDRESS?1U:2U);i++){
        if(i2c_master_probe(bus,candidates[i],20)!=ESP_OK) continue;
        i2c_device_config_t cfg={.dev_addr_length=I2C_ADDR_BIT_LEN_7,
            .device_address=candidates[i],.scl_speed_hz=100000};
        if(i2c_master_bus_add_device(bus,&cfg,&device)!=ESP_OK){device=NULL;continue;}
        d->address=active_address=candidates[i];
        if(oled_text_init(d)){ESP_LOGI(TAG,"OLED ACK at 0x%02x; panel type must match configuration",active_address);return true;}
        i2c_master_bus_rm_device(device);device=NULL;
    }
    return false;
}
static const char *motion_name(uint8_t motion)
{
    return motion==1?"FWD":motion==2?"REV":"STOP";
}
static void display_task(void *arg)
{
    (void)arg;
    if(!pins_valid()){ESP_LOGE(TAG,"OLED pins conflict; display disabled");vTaskDelete(NULL);return;}
    i2c_master_bus_config_t cfg={.i2c_port=I2C_NUM_0,
        .sda_io_num=CONFIG_ROBOT_OLED_SDA_GPIO,.scl_io_num=CONFIG_ROBOT_OLED_SCL_GPIO,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true};
    esp_err_t err=i2c_new_master_bus(&cfg,&bus);
    if(err!=ESP_OK){ESP_LOGW(TAG,"OLED bus unavailable: %s",esp_err_to_name(err));vTaskDelete(NULL);return;}
    oled_text_t d={.write=write_bus,.height=CONFIG_ROBOT_OLED_HEIGHT,
#ifdef CONFIG_ROBOT_OLED_SH1106
        .sh1106=true,.column_offset=2
#else
        .sh1106=false,.column_offset=0
#endif
    };
    vTaskDelay(pdMS_TO_TICKS(100));
    for(;;){
        if(!online) online=connect_display(&d);
        if(online){
            oled_status_t s;portENTER_CRITICAL(&display_lock);s=latest;portEXIT_CRITICAL(&display_lock);
            char lines[8][22]={{0}};
            snprintf(lines[0],22,"ESP32-S3 X AXIS");
            snprintf(lines[1],22,"BLE %s STM %s",s.ble?"ON":"OFF",s.stm_online?"ON":"OFF");
            snprintf(lines[2],22,"SRC %s",s.source==1?"BLE":s.source==2?"ESP-NOW":"NONE");
            snprintf(lines[3],22,"CMD %s S%u",motion_name(s.motion),s.speed);
            snprintf(lines[4],22,"ACK %s",!s.stm_online?"OFFLINE":s.confirmed?motion_name(s.stm_motion):"WAIT");
            if(!s.capture_available) snprintf(lines[5],22,"PUL CAPTURE OFF");
            else if(!s.pulse_recent) snprintf(lines[5],22,"PUL NO RECENT EDGES");
            else snprintf(lines[5],22,"PUL %lu HZ D%u",(unsigned long)s.pulse_hz,s.direction);
            snprintf(lines[6],22,"ERR E%u STM %u",s.error,s.stm_online?s.stm_error:0);
            snprintf(lines[7],22,"UP %lu S",(unsigned long)s.uptime);
            if(!oled_text_frame(&d,(const char (*)[22])lines,s.uptime/3U)){
                online=false;ESP_LOGW(TAG,"OLED transfer failed; retry in 3 s");
            }
        }
        vTaskDelay(pdMS_TO_TICKS(online?CONFIG_ROBOT_OLED_PERIOD_MS:3000));
    }
}
void oled_status_start(void)
{
    if(xTaskCreate(display_task,"oled",3072,NULL,1,NULL)!=pdPASS)
        ESP_LOGW(TAG,"OLED task unavailable; control continues");
}
#else
void oled_status_publish(const oled_status_t *s){(void)s;}
void oled_status_start(void){}
#endif
