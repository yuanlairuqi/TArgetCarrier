// Pin map and initialization sequence based on Waveshare ESP32-S3-GEEK official demo.
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "panel.h"
#include "sdkconfig.h"
#include "font5x7.h"


// ST7789 135x240 glass in landscape: MADCTL and offsets are a single profile.
// Waveshare bundled TFT_eSPI ST7789_Rotation.h, landscape rotation 3.
#define W 240
#define H 135
#define X_GAP 40
#define Y_GAP 52
#define MADCTL 0xA0
static spi_device_handle_t spi;
static uint16_t pixels[W*H];
static uint16_t rgb565(uint32_t c) {return ((c>>8)&0xf800)|((c>>5)&0x7e0)|((c>>3)&31);}
static uint16_t swapped(uint16_t c) {return (c<<8)|(c>>8);}
static void transfer(const void *data, size_t len, int dc) {
    gpio_set_level(8,dc);
    spi_transaction_t t={.length=len*8,.tx_buffer=data};
    ESP_ERROR_CHECK(spi_device_polling_transmit(spi,&t));
}
static void cmd(uint8_t c,const uint8_t *p,size_t n) {transfer(&c,1,0); if(n)transfer(p,n,1);}
static void rect(int x,int y,int w,int h,uint16_t color) {
    for(int j=y;j<y+h&&j<H;j++) for(int i=x;i<x+w&&i<W;i++) if(i>=0&&j>=0)pixels[j*W+i]=swapped(color);
}
static void text_at(int x,int y,const char *s,int scale,uint16_t color) {
    for(;*s&&x+5*scale<=W;s++,x+=6*scale) {
        unsigned char c=(unsigned char)*s;
        if(c<32||c>126)c='?';
        for(int j=0;j<8;j++) for(int i=0;i<5;i++)
            if(font_row((char)c,j)&(0x10>>i)) rect(x+i*scale,y+j*scale,scale,scale,color);
    }
}
void panel_init(void) {
    gpio_config_t io={.pin_bit_mask=(1ULL<<7)|(1ULL<<8)|(1ULL<<9),.mode=GPIO_MODE_OUTPUT};
    ESP_ERROR_CHECK(gpio_config(&io));
    gpio_set_level(7,0); // Keep backlight off until the first complete frame.
    spi_bus_config_t bus={.mosi_io_num=11,.miso_io_num=-1,.sclk_io_num=12,.quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=4096};
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    spi_device_interface_config_t device={.clock_speed_hz=20000000,.mode=3,.spics_io_num=10,.queue_size=1};
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST,&device,&spi));
    gpio_set_level(9,0); vTaskDelay(pdMS_TO_TICKS(20)); gpio_set_level(9,1); vTaskDelay(pdMS_TO_TICKS(120));
    const uint8_t mad[]={MADCTL}, mode[]={0x05}, porch[]={12,12,0,0x33,0x33};
    cmd(0x36,mad,1); cmd(0x3a,mode,1); cmd(0xb2,porch,5);
    const uint8_t init[][2]={{0xb7,0x35},{0xbb,0x19},{0xc0,0x2c},{0xc2,1},{0xc3,0x12},{0xc4,0x20},{0xc6,0x0f}};
    for(unsigned i=0;i<sizeof(init)/sizeof(init[0]);i++)cmd(init[i][0],&init[i][1],1);
    const uint8_t power[]={0xa4,0xa1}; cmd(0xd0,power,2);
    const uint8_t pos[]={0xd0,4,13,17,19,43,63,84,76,24,13,11,31,35};
    const uint8_t neg[]={0xd0,4,12,17,19,44,63,68,81,47,31,31,32,35};
    cmd(0xe0,pos,14);cmd(0xe1,neg,14);cmd(0x21,NULL,0);cmd(0x11,NULL,0);
    vTaskDelay(pdMS_TO_TICKS(120));cmd(0x29,NULL,0);
    ledc_timer_config_t timer={.speed_mode=LEDC_LOW_SPEED_MODE,.timer_num=LEDC_TIMER_0,.duty_resolution=LEDC_TIMER_10_BIT,.freq_hz=5000,.clk_cfg=LEDC_AUTO_CLK};
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t channel={.gpio_num=7,.speed_mode=LEDC_LOW_SPEED_MODE,.channel=LEDC_CHANNEL_0,.timer_sel=LEDC_TIMER_0,.duty=0};
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}
void panel_draw(bool connected,bool armed,int value,const char *text,uint32_t rgb,int brightness) {
    const uint16_t white=0xffff, muted=0xad75;
    if(value<0)value=0;
    if(value>100)value=100;
    if(brightness<0)brightness=0;
    if(brightness>100)brightness=100;
    rect(0,0,W,H,rgb565(rgb));
    rect(0,0,W,25,0x1948);
    text_at(8,5,"GEEK LINK",2,white);
    text_at(174,9,connected?"USB LIVE":"USB WAIT",1,connected?0x07e0:0xffe0);
    char target[32];
    snprintf(target,sizeof(target),"NODE %u   CH %d",(unsigned)CONFIG_ROBOT_ESPNOW_NODE_ID,CONFIG_ROBOT_ESPNOW_CHANNEL);
    text_at(10,34,target,1,muted);
    text_at(10,51,text,2,white);
    rect(10,78,220,1,0x4208);
    char speed[24];snprintf(speed,sizeof(speed),"CMD %d%%",value);
    text_at(10,89,speed,2,white);
    text_at(142,91,armed?"STM ACK":"STM WAIT",1,armed?0x07e0:0xffe0);
    rect(10,111,220,3,0x4208);
    rect(10,111,value*220/100,3,0x07ff);
    text_at(10,122,"GPIO != MOTOR POSITION",1,muted);
    // Both dimensions and both offsets follow the landscape MADCTL profile.
    const unsigned xe=X_GAP+W-1, ye=Y_GAP+H-1;
    const uint8_t xs[]={X_GAP>>8,X_GAP&255,xe>>8,xe&255};
    const uint8_t ys[]={Y_GAP>>8,Y_GAP&255,ye>>8,ye&255};
    cmd(0x2a,xs,4);cmd(0x2b,ys,4);cmd(0x2c,NULL,0);
    const uint8_t *p=(const uint8_t*)pixels;size_t n=sizeof(pixels);
    while(n){size_t chunk=n>4096?4096:n;transfer(p,chunk,1);p+=chunk;n-=chunk;}
    ESP_ERROR_CHECK(ledc_set_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0,brightness*1023/100));
    ESP_ERROR_CHECK(ledc_update_duty(LEDC_LOW_SPEED_MODE,LEDC_CHANNEL_0));
}
