
#include <assert.h>
#include <stdio.h>
#include "esp_bus.c"
static void frame(uint8_t*b,uint8_t type,uint32_t seq){tc_frame f={.type=type,.session=9,.seq=seq,.total=1,.len=1,.count=1};f.data[0]=12;assert(tc_encode(&f,b));}
int main(void){assert(tc_bus_init());uint8_t high[128],low[128],result[128];frame(high,TC_WRITE,1);frame(low,TC_WAVE,2);frame(mock_rx,TC_RESULT,1);assert(tc_bus_send(low,0));assert(tc_bus_send(high,0));
#if TC_BACKEND==TC_UART
 mock_uart_count=128;tc_bus_poll(1);assert(mock_writes==1&&!memcmp(mock_tx,high,128));assert(tc_bus_receive(result,1)&&result[3]==TC_RESULT);tc_bus_poll(2);assert(mock_tx[3]==TC_WAVE);puts("PASS ESP UART: stream parse, TX scheduling, high-priority command before waveform");
#elif TC_BACKEND==TC_SPI
 assert(mock_spi==0);
 tc_bus_poll(10);assert(mock_spi==1 && spi_inflight && !memcmp(mock_tx,high,128));
 tc_bus_poll(20);assert(mock_spi==2 && spi_inflight && !memcmp(mock_tx,high,128));
 tc_frame ack={.type=TC_HELLO,.session=9,.seq=1,.total=6,.len=6,.count=1};
 ack.data[0]='S';ack.data[1]='A';tc_put32(ack.data+2,tc_get32(high+124)^1);assert(tc_encode(&ack,mock_rx));
 tc_bus_poll(30);assert(spi_inflight); /* Wrong frame CRC cannot ACK. */
 tc_put32(ack.data+2,tc_get32(high+124));assert(tc_encode(&ack,mock_rx));
 tc_bus_poll(40);assert(!spi_inflight);
 memset(mock_rx,0,128);tc_bus_poll(50);assert(spi_inflight && !memcmp(mock_tx,low,128));
 mock_fail=1;tc_bus_poll(60);assert(stats.errors==1 && spi_inflight);
 mock_fail=0;tc_bus_poll(151);assert(!spi_inflight && stats.tx_drop==1);
 assert(tc_bus_send(high,160));tc_bus_poll(160);assert(spi_inflight);
 tc_bus_cancel_pending();assert(!spi_inflight && !out.used && !outlo.used);
 puts("PASS SPI four-wire ESP: repeated send, matched receipt ACK, CRC mismatch, driver failure, bounded expiry, STOP cancel");
#else
 tc_bus_poll(10);assert(mock_writes==1&&mock_ack==1&&!memcmp(mock_tx,high,128));assert(tc_bus_receive(result,10));mock_rx[124]^=1;tc_bus_poll(20);assert(mock_ack==1);mock_fail=1;tc_bus_poll(30);assert(stats.errors>0);puts("PASS ESP I2C: separate write/read/ACK transactions, no ACK on corrupt payload, priority and driver error");
#endif
return 0;}
