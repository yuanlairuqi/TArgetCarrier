// Production radio queue code with SDK/RTOS mocks. Does NOT validate radio/HMAC hardware.
#include <assert.h>
#include <stdio.h>
#include "now_radio.c"
static void reset_test(void){
    if(!control_tx)control_tx=xQueueCreate(4,sizeof(tx_item_t));
    if(!wave_tx)wave_tx=xQueueCreate(1,sizeof(tx_item_t));
    xQueueReset(control_tx);xQueueReset(wave_tx);ready=true;sending=false;mock_time=0;mock_sends=0;mock_send_error=0;
}
int main(void){
    uint8_t motion[64]={'W','N',1,2},stop[64]={'W','N',1,2},status[64]={'W','N',1,3},wave[64]={'W','N',1,4};motion[25]=1;
    reset_test();assert(now_radio_send_wave(wave));now_radio_send(status);now_radio_service();assert(mock_sent[3]==3);
    assert(!now_radio_send_wave(wave));send_cb(NULL,0);now_radio_service();assert(mock_sent[3]==4); // one existing wave slot only
    reset_test();now_radio_send(motion);now_radio_send(motion);assert(!now_radio_send_wave(wave));now_radio_send(stop);
    assert(uxQueueMessagesWaiting(control_tx)==1);now_radio_service();assert(mock_sent[25]==0&&mock_sends==1);
    send_cb(NULL,0);now_radio_service();assert(mock_sends==1);
    reset_test();now_radio_send(motion);mock_time=101000;now_radio_service();assert(mock_sends==0);
    reset_test();now_radio_send(stop);mock_time=101000;now_radio_service();assert(mock_sends==1&&mock_sent[25]==0);
    reset_test();assert(now_radio_send_wave(wave));mock_time=101000;now_radio_service();assert(mock_sends==0);
    reset_test();now_radio_send(status);now_radio_service();now_radio_send(stop);now_radio_service();assert(mock_sends==1);
    send_cb(NULL,1);now_radio_service();assert(mock_sends==2&&mock_sent[25]==0);
    reset_test();mock_send_error=1;now_radio_send(motion);now_radio_service();assert(!sending);now_radio_service();assert(mock_sends==1);
    reset_test();for(unsigned i=0;i<20;i++){status[20]=(uint8_t)i;now_radio_send(status);}assert(uxQueueMessagesWaiting(control_tx)==4);
    now_radio_send(stop);now_radio_service();assert(mock_sent[25]==0);
    puts("PASS: 8 production radio queue groups (SDK/RTOS mocked; no RF/HMAC proof)");
}
