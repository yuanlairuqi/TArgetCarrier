/* In-process bus simulation uses the actual two platform drivers with mocks.
 * It does not simulate GPIO timing, interrupts, or electrical behavior. */
#include <assert.h>
#include <stdio.h>
#define tc_bus_init esp_bus_init
#define tc_bus_send esp_bus_send
#define tc_bus_poll esp_bus_poll
#define tc_bus_receive esp_bus_receive
#define tc_bus_get_stats esp_bus_get_stats
#define tc_bus_cancel_pending esp_bus_cancel_pending
#define tx esp_tx
#define rx esp_rx
#define stats esp_stats
#define stream esp_stream
#define accept esp_accept
#include "esp_bus.c"
#undef tc_bus_init
#undef tc_bus_send
#undef tc_bus_poll
#undef tc_bus_receive
#undef tc_bus_get_stats
#undef tc_bus_cancel_pending
#undef tx
#undef rx
#undef stats
#undef stream
#undef accept
#include "stm32_bus.c"
static void frame(uint8_t *b, uint32_t seq, bool stop) {
    tc_frame f={.type=TC_CONTROL,.session=17,.seq=seq,.total=9,.len=9,.count=1};
    f.data[0]=0x4c;f.data[1]=1;f.data[2]=1;f.data[3]=stop?1:0;
    assert(tc_encode(&f,b));
}
static void cycle(unsigned now, bool accept_transaction, bool corrupt_reply) {
    memcpy(mock_rx,tx,128);
    if(corrupt_reply)mock_rx[124]^=1;
    tc_bus_poll(now);
    esp_bus_poll(now);
    if(accept_transaction){
        memcpy(rx,mock_tx,128); DMA1_Channel2->CNDTR=0;EXTI->PR=1u<<4;EXTI4_IRQHandler();
    }
}
int main(void) {
    assert(esp_bus_init() && tc_bus_init());
    uint8_t cmd[128],stop[128],status[128],outbuf[128];
    frame(cmd,1,false);assert(esp_bus_send(cmd,0));
    cycle(2,false,false);assert(spi_inflight); /* STM was not armed, so no receipt. */
    cycle(4,true,false);assert(spi_inflight);
    cycle(6,true,true);assert(spi_inflight); /* Damaged ACK must not acknowledge. */
    cycle(8,true,false);assert(!spi_inflight);
    assert(tc_bus_receive(outbuf,8)&&!memcmp(outbuf,cmd,128));
    assert(!tc_bus_receive(outbuf,8)); /* Retries did not duplicate application delivery. */
    tc_frame st={.type=TC_STATUS,.session=17,.seq=50,.total=1,.len=1,.count=1};
    st.data[0]=42;assert(tc_encode(&st,status));assert(tc_bus_send(status,9));
    cycle(10,true,false);cycle(12,true,false);
    assert(esp_bus_receive(outbuf,12)&&!memcmp(outbuf,status,128));
    frame(cmd,2,false);frame(stop,3,true);assert(esp_bus_send(cmd,12));cycle(14,true,false);
    esp_bus_cancel_pending();assert(esp_bus_send(stop,14));cycle(16,true,false);
    assert(spi_inflight); /* Old ACK for seq 2 cannot confirm the STOP seq 3. */
    cycle(18,true,false);assert(!spi_inflight);
    assert(tc_bus_receive(outbuf,18)&&!memcmp(outbuf,stop,128));
    assert(!tc_bus_receive(outbuf,18));
    puts("PASS paired real driver model: missed transaction, corrupt ACK, retry dedup, status, STOP preemption and late ACK");
}
