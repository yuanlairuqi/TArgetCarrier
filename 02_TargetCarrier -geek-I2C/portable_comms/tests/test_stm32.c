#include <assert.h>
#include <stdio.h>
#include "../platform/stm32_bus.c"
static void frame(uint8_t *b, uint8_t type, uint32_t seq) {
    tc_frame f = {.type = type, .session = 7, .seq = seq, .total = 1, .len = 1, .count = 1};
    f.data[0] = 42;
    assert(tc_encode(&f, b));
}
#if TC_BACKEND == TC_I2C
static void write_transaction(const uint8_t *b, int n) {
    I2C2->SR1 = I2C_SR1_ADDR;
    I2C2->SR2 = 0;
    I2C2_EV_IRQHandler();
    for (int i = 0; i < n; i++) {
        I2C2->DR = b[i];
        I2C2->SR1 = I2C_SR1_RXNE;
        I2C2_EV_IRQHandler();
    }
    I2C2->SR1 = I2C_SR1_STOPF;
    I2C2_EV_IRQHandler();
    I2C2->SR1 = 0;
}
#endif
int main(void) {
    assert(tc_bus_init());
    uint8_t b[128], out[128];
    frame(b, TC_READ, 10);
#if TC_BACKEND == TC_UART
    memcpy(uart_rx, b, 128);
    DMA1_Channel3->CNDTR = 384;
    tc_bus_poll(1);
    assert(tc_bus_receive(out, 1) && !memcmp(b, out, 128));
    assert(tc_bus_send(b, 2));
    tc_bus_poll(2);
    assert(DMA1_Channel2->CNDTR == 128 && !memcmp(tx, b, 128));
    wraps = 2;
    DMA1_Channel3->CNDTR = 512;
    tc_bus_poll(3);
    assert(stats.rx_drop == 1 && consumed == 1024);
    DMA1->ISR = DMA_ISR_TEIF3;
    DMA1_Channel3_IRQHandler();
    assert(!(DMA1_Channel3->CCR & DMA_CCR_EN));
    DMA1->ISR = 0;
    tc_bus_poll(4);
    assert((DMA1_Channel3->CCR & DMA_CCR_EN) && DMA1_Channel3->CNDTR == 512);
    DMA1->ISR = DMA_ISR_TEIF2;
    tc_bus_poll(5);
    assert(DMA1_Channel2->CNDTR == 0 && stats.errors == 2);
    DMA1->ISR = 0;
    puts("PASS UART production adapter: DMA RX parse, DMA TX ownership, overrun discards stale bytes, DMA error recovery");
#elif TC_BACKEND == TC_SPI
    assert(DMA1_Channel2->CNDTR == 128);
    memcpy(rx, b, 128);
    DMA1_Channel2->CNDTR = 0;
    EXTI->PR = 1u << 4;
    EXTI4_IRQHandler();
    assert(tc_bus_receive(out, 0) && !memcmp(out, b, 128));
    assert(DMA1_Channel2->CNDTR == 128);
    assert(tc_bus_send(b, 0));
    memset(rx, 0, 128);
    DMA1_Channel2->CNDTR = 0;
    EXTI4_IRQHandler();
    assert(!memcmp(tx, b, 128));
    DMA1_Channel2->CNDTR = 1;
    EXTI4_IRQHandler();
    assert(stats.short_transaction == 1 && !memcmp(tx, b, 128));
    for (int i = 0; i < TC_Q + 2; i++) {
        memcpy(rx, b, 128);
        DMA1_Channel2->CNDTR = 0;
        EXTI4_IRQHandler();
    }
    assert(stats.rx_drop == 2);
    puts("PASS SPI production adapter: CS-boundary DMA, exact length, short transaction retains "
         "TX, RX overflow");
#else
    write_transaction(b, 128);
    assert(tc_bus_receive(out, 0) && !memcmp(b, out, 128));
    write_transaction(b, 40);
    assert(stats.short_transaction == 1 && !tc_bus_receive(out, 0));
    assert(tc_bus_send(b, 1));
    tc_bus_poll(1);
    assert(tx_valid);
    I2C2->SR1 = I2C_SR1_ADDR;
    I2C2->SR2 = I2C_SR2_TRA;
    I2C2_EV_IRQHandler();
    for (int i = 0; i < 128; i++) {
        I2C2->SR1 = I2C_SR1_TXE;
        I2C2_EV_IRQHandler();
        assert(I2C2->DR == b[i]);
    }
    I2C2->SR1 = I2C_SR1_AF;
    I2C2_ER_IRQHandler();
    assert(tx_valid);
    uint8_t ack[12] = {'A', 'K', 2, 0};
    tc_put32(ack + 4, 10);
    tc_put32(ack + 8, tc_get32(b + 124) ^ 1);
    write_transaction(ack, 12);
    assert(tx_valid);
    tc_put32(ack + 8, tc_get32(b + 124));
    write_transaction(ack, 12);
    assert(!tx_valid);
    transaction_at = 0;
    transaction_active = true;
    tc_bus_poll(51);
    assert(stats.errors == 1);
    puts("PASS I2C production adapter: address/write/read/STOP/NACK, short write, explicit "
         "matching mailbox ACK, timeout reset");
#endif
    return 0;
}
