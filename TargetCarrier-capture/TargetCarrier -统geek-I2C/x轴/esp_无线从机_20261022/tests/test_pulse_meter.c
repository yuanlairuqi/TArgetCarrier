#include "pulse_meter.h"
#include <assert.h>
#include <stdio.h>
int main(void)
{
    pulse_meter_t m={0};uint32_t hz=123;
    assert(!pulse_meter_read(&m,1000000,0,&hz) && hz==0);
    pulse_meter_edge(&m,100,10000);assert(!pulse_meter_read(&m,1000000,10001,&hz));
    pulse_meter_edge(&m,10100,20000);assert(pulse_meter_read(&m,1000000,20000,&hz) && hz==100);
    puts("PASS frequency requires two rising edges");
    assert(!pulse_meter_read(&m,1000000,520001,&hz) && hz==0);
    pulse_meter_edge(&m,1000100,1010000);assert(!pulse_meter_read(&m,1000000,1010000,&hz));
    pulse_meter_edge(&m,1010100,1020000);assert(pulse_meter_read(&m,1000000,1020000,&hz) && hz==100);
    puts("PASS stale samples and first edge after idle cannot reuse old frequency");
    m=(pulse_meter_t){0};pulse_meter_edge(&m,0xffffff00U,2000);
    pulse_meter_edge(&m,0x000002e8U,3000);assert(pulse_meter_read(&m,1000000,3000,&hz) && hz==1000);
    puts("PASS 32-bit capture counter wrap");
    pulse_meter_edge(&m,0x000002e8U,3001);assert(!pulse_meter_read(&m,1000000,3001,&hz));
    assert(!pulse_meter_read(&m,0,3001,&hz));assert(!pulse_meter_read(&m,1000000,1000,&hz));
    puts("PASS zero-period and invalid timestamps rejected");
}
