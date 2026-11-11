
#include <assert.h>
#include <stdio.h>
#include "esp_bus.c"
static void frame(uint8_t*b,uint8_t type,uint32_t seq){tc_frame f={.type=type,.session=9,.seq=seq,.total=1,.len=1,.count=1};f.data[0]=12;assert(tc_encode(&f,b));}
int main(void){assert(tc_bus_init());uint8_t high[128],low[128],result[128];frame(high,TC_WRITE,1);frame(low,TC_WAVE,2);frame(mock_rx,TC_RESULT,1);assert(tc_bus_send(low,0));assert(tc_bus_send(high,0));
#if TC_BACKEND==TC_UART
 mock_uart_count=128;tc_bus_poll(1);assert(mock_writes==1&&!memcmp(mock_tx,high,128));assert(tc_bus_receive(result,1)&&result[3]==TC_RESULT);tc_bus_poll(2);assert(mock_tx[3]==TC_WAVE);puts("PASS ESP UART: stream parse, TX scheduling, high-priority command before waveform");
#elif TC_BACKEND==TC_SPI
 mock_ready=0;tc_bus_poll(10);assert(mock_spi==0&&out.used==1);mock_ready=1;tc_bus_poll(20);assert(mock_spi==1&&!memcmp(mock_tx,high,128));assert(tc_bus_receive(result,20));mock_fail=1;tc_bus_poll(30);assert(stats.errors==1);puts("PASS ESP SPI: READY gates queue consumption, exact full-duplex transaction, priority and driver error");
#else
 tc_bus_poll(10);assert(mock_writes==1&&mock_ack==1&&!memcmp(mock_tx,high,128));assert((mock_min_timeout*100/1000)>=2);assert(tc_bus_receive(result,10));mock_rx[124]^=1;tc_bus_poll(20);assert(mock_ack==1);mock_fail=1;tc_bus_poll(30);assert(stats.errors>0);puts("PASS ESP I2C: separate write/read/ACK transactions, no ACK on corrupt payload, priority and driver error");
#endif
return 0;}
