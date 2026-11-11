/* Compatibility API name; actual STM32 transport is selected SPI/I2C, not UART. */
#include "uart_link.h"
#include "wl_esp.h"
bool uart_link_init(void){return wl_esp_init();}
int uart_link_read(uint8_t*d,size_t n){return wl_esp_read(d,n);}
bool uart_link_send(const wire_frame_t*f){uint8_t d[WIRE_SIZE];wire_encode(f,d);bool stop=(f->type==WIRE_COMMAND||f->type==WIRE_PROFILE)&&(f->payload[0]==0||f->payload[0]==3);return wl_esp_send(d,sizeof(d),stop);}
