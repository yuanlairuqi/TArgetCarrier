/* STM32F103xB CMSIS register adapter. Independent reference firmware only.
 * No RTOS calls in IRQ. Main owns protocol; IRQ owns transaction buffers.
 * Exactly one bus is compiled. HCLK/PCLK1/PCLK2 = 72/36/72 MHz. */
#include "tc_bus.h"
#include "tc_board.h"
#include "stm32f1xx.h"
#include <string.h>
static tc_queue txq, txlo, rxhi, rxlo;
static tc_stream stream;
static tc_bus_stats stats;
static uint8_t tx[128], rx[128];
static volatile uint8_t tx_valid;
static volatile uint16_t pos;
static volatile uint32_t now_ms;
static volatile uint32_t wraps;
static uint32_t consumed;
static uint8_t uart_rx[512];
static uint32_t lock(void) {
    uint32_t p = __get_PRIMASK();
    __disable_irq();
    return p;
}
static void unlock(uint32_t p) {
    __set_PRIMASK(p);
}
static void ready(bool b) {
    GPIOB->BSRR = b ? (1u << 1) : (1u << 17);
}
static void pin(GPIO_TypeDef *g, unsigned n, unsigned mode) {
    volatile uint32_t *r = n < 8 ? &g->CRL : &g->CRH;
    unsigned s = (n % 8) * 4;
    *r = (*r & ~(15u << s)) | (mode << s);
}
static void accept(void) {
    tc_frame f;
    if (tc_decode(rx, &f)) {
        tc_queue *q = f.type == TC_WAVE ? &rxlo : &rxhi;
        if (!tc_push(q, rx, now_ms, f.type == TC_WAVE))
            stats.rx_drop++;
    }
}
#if TC_BACKEND == TC_SPI
static void arm(void) {
    DMA1_Channel2->CCR = 0;
    DMA1_Channel3->CCR = 0;
    SPI1->CR1 &= ~SPI_CR1_SPE;
    while (SPI1->SR & SPI_SR_RXNE) {
        volatile uint32_t x = SPI1->DR;
        (void)x;
    }
    volatile uint32_t x = SPI1->SR;
    (void)x;
    DMA1_Channel2->CPAR = (uint32_t)&SPI1->DR;
    DMA1_Channel2->CMAR = (uint32_t)rx;
    DMA1_Channel2->CNDTR = 128;
    DMA1_Channel3->CPAR = (uint32_t)&SPI1->DR;
    DMA1_Channel3->CMAR = (uint32_t)tx;
    DMA1_Channel3->CNDTR = 128;
    DMA1->IFCR = DMA_IFCR_CGIF2 | DMA_IFCR_CGIF3;
    DMA1_Channel2->CCR = DMA_CCR_MINC | DMA_CCR_PL_1 | DMA_CCR_EN;
    DMA1_Channel3->CCR = DMA_CCR_MINC | DMA_CCR_PL_1 | DMA_CCR_DIR | DMA_CCR_EN;
    SPI1->CR1 |= SPI_CR1_SPE;
    ready(true);
}
static void spi_finish(void) {
    ready(false);
    DMA1_Channel2->CCR &= ~DMA_CCR_EN;
    DMA1_Channel3->CCR &= ~DMA_CCR_EN;
    if (DMA1_Channel2->CNDTR == 0 && !(SPI1->SR & SPI_SR_OVR) &&
        !(DMA1->ISR & (DMA_ISR_TEIF2 | DMA_ISR_TEIF3))) {
        accept();
        tx_valid = 0;
        memset(tx, 0, 128);
        if ((tc_pop(&txq, tx, now_ms, 100) || tc_pop(&txlo, tx, now_ms, 100)))
            tx_valid = 1;
    } else
        stats.short_transaction++;
    arm();
}

#ifdef WL_ARDUINO_SPI_CALLBACK
void wl_spi_cs_rising(void){spi_finish();}
#else
void EXTI4_IRQHandler(void){if(EXTI->PR&(1u<<4)){EXTI->PR=1u<<4;spi_finish();}}
#endif
#elif TC_BACKEND == TC_I2C
static bool reading;
static volatile bool transaction_active;
static uint8_t i2c_tx[128];
static volatile uint32_t transaction_at;
static void i2c_reset(void) {
    I2C2->CR1 = I2C_CR1_SWRST;
    I2C2->CR1 = 0;
    I2C2->CR2 = 36 | I2C_CR2_ITEVTEN | I2C_CR2_ITBUFEN | I2C_CR2_ITERREN;
    I2C2->OAR1 = (1u << 14) | (TC_I2C_ADDR << 1);
    #if TC_I2C_HZ == 400000
    I2C2->CCR = I2C_CCR_FS | 30u; /* 36MHz/(3*400k), duty 2 */
    I2C2->TRISE = 11;
#else
    I2C2->CCR = 180;
    I2C2->TRISE = 37;
#endif
    I2C2->CR1 = I2C_CR1_PE | I2C_CR1_ACK;
    pos = 0;
}
void I2C2_EV_IRQHandler(void) {
    uint32_t sr = I2C2->SR1;
    if (sr & I2C_SR1_ADDR) {
        uint32_t sr2 = I2C2->SR2;
        reading = (sr2 & I2C_SR2_TRA) != 0;
        pos = 0;
        transaction_at = now_ms;
        transaction_active = true;
        if (reading)
            memcpy(i2c_tx, tx, 128);
    }
    if (!reading && (sr & I2C_SR1_RXNE)) {
        uint8_t v = I2C2->DR;
        if (pos < 128)
            rx[pos] = v;
        pos++;
    }
    if (reading && (sr & I2C_SR1_TXE)) {
        I2C2->DR = pos < 128 ? i2c_tx[pos] : 0;
        if (pos < 129)
            pos++;
    }
    if (sr & I2C_SR1_STOPF) {
        I2C2->CR1 |= I2C_CR1_PE;
        if (!reading && pos == 128)
            accept();
        else if (!reading && pos == 12 && rx[0] == 'A' && rx[1] == 'K' && rx[2] == 2 &&
                 rx[3] == 0 && tx_valid && tc_get32(rx + 4) == tc_get32(tx + 8) &&
                 tc_get32(rx + 8) == tc_get32(tx + 124)) {
            tx_valid = 0;
            ready(false);
        } else if (!reading)
            stats.short_transaction++;
        transaction_at = 0;
        transaction_active = false;
        pos = 0;
    }
}
void I2C2_ER_IRQHandler(void) {
    uint32_t sr = I2C2->SR1;
    if (sr & I2C_SR1_AF) {
        if (!reading || pos < 128)
            stats.short_transaction++; /* A read never consumes mailbox: explicit AK/2 transaction
                                          does. */
    }
    if (sr & (I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR))
        stats.errors++;
    I2C2->SR1 &= ~(I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR);
    transaction_at = 0;
    transaction_active = false;
    pos = 0;
}
#else
void DMA1_Channel3_IRQHandler(void) {
    uint32_t flags = DMA1->ISR;
    if (flags & DMA_ISR_TCIF3)
        wraps++;
    if (flags & DMA_ISR_TEIF3) {
        stats.errors++;
        DMA1_Channel3->CCR &= ~DMA_CCR_EN;
    }
    DMA1->IFCR = DMA_IFCR_CGIF3;
}
#endif
bool tc_bus_init(void) {
    RCC->APB2ENR |= RCC_APB2ENR_IOPAEN | RCC_APB2ENR_IOPBEN | RCC_APB2ENR_AFIOEN;
    RCC->AHBENR |= RCC_AHBENR_DMA1EN;
    pin(GPIOB, 1, 2);
    ready(false);
#if TC_BACKEND == TC_UART
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    pin(GPIOB, 10, 0xb);
    pin(GPIOB, 11, 4);
    USART3->BRR = (36000000 + TC_UART_BAUD / 2) / TC_UART_BAUD;
    USART3->CR3 = USART_CR3_DMAR | USART_CR3_DMAT;
    USART3->CR1 = USART_CR1_UE | USART_CR1_TE | USART_CR1_RE;
    DMA1_Channel3->CPAR = (uint32_t)&USART3->DR;
    DMA1_Channel3->CMAR = (uint32_t)uart_rx;
    DMA1_Channel3->CNDTR = 512;
    DMA1_Channel3->CCR = DMA_CCR_MINC | DMA_CCR_CIRC | DMA_CCR_TCIE | DMA_CCR_TEIE | DMA_CCR_EN;
    NVIC_SetPriority(DMA1_Channel3_IRQn, 5);
    NVIC_EnableIRQ(DMA1_Channel3_IRQn);
#elif TC_BACKEND == TC_SPI
    RCC->APB2ENR |= RCC_APB2ENR_SPI1EN;
    pin(GPIOA, 4, 8);
    GPIOA->BSRR = 1u << 4;
    pin(GPIOA, 5, 4);
    pin(GPIOA, 6, 0xb);
    pin(GPIOA, 7, 4);
    SPI1->CR1 = 0;
    SPI1->CR2 = SPI_CR2_RXDMAEN | SPI_CR2_TXDMAEN;
    AFIO->EXTICR[1] &= ~15u;
    EXTI->RTSR |= 1u << 4;
    EXTI->IMR |= 1u << 4;
    EXTI->PR = 1u << 4;
    NVIC_SetPriority(EXTI4_IRQn, 5);
    NVIC_EnableIRQ(EXTI4_IRQn);
    arm();
#else
    RCC->APB1ENR |= RCC_APB1ENR_I2C2EN;
    pin(GPIOB, 10, 0xf);
    pin(GPIOB, 11, 0xf);
    i2c_reset();
    NVIC_SetPriority(I2C2_EV_IRQn, 5);
    NVIC_SetPriority(I2C2_ER_IRQn, 5);
    NVIC_EnableIRQ(I2C2_EV_IRQn);
    NVIC_EnableIRQ(I2C2_ER_IRQn);
#endif
    return true;
}
bool tc_bus_send(const uint8_t *b, uint32_t now) {
    uint32_t p = lock();
    bool ok = tc_push(b[3] == TC_WAVE ? &txlo : &txq, b, now, b[3] == TC_WAVE);
    if (!ok)
        stats.tx_drop++;
    unlock(p);
    return ok;
}
void tc_bus_poll(uint32_t now) {
    now_ms = now;
#if TC_BACKEND == TC_UART
    if (!(DMA1_Channel3->CCR & DMA_CCR_EN)) {
        uint32_t p = lock();
        wraps = consumed = 0;
        stream.used = 0;
        DMA1_Channel3->CNDTR = 512;
        DMA1->IFCR = DMA_IFCR_CGIF3;
        DMA1_Channel3->CCR |= DMA_CCR_EN;
        unlock(p);
    }
    uint32_t p = lock();
    if (DMA1->ISR & DMA_ISR_TCIF3) {
        wraps++;
        DMA1->IFCR = DMA_IFCR_CGIF3;
    }
    uint32_t produced = wraps * 512 + 512 - DMA1_Channel3->CNDTR;
    unlock(p);
    if (produced - consumed > 512) {
        stats.rx_drop++;
        consumed = produced;
        stream.used = 0;
    }
    while (consumed != produced) {
        uint8_t b[128];
        if (tc_feed(&stream, uart_rx[consumed++ % 512], b)) {
            memcpy(rx, b, 128);
            accept();
        }
    }
    if (USART3->SR & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) {
        volatile uint32_t v = USART3->DR;
        (void)v;
        stats.errors++;
        stream.used = 0;
    }
    if (DMA1->ISR & DMA_ISR_TEIF2) {
        DMA1_Channel2->CCR = 0;
        DMA1_Channel2->CNDTR = 0;
        DMA1->IFCR = DMA_IFCR_CGIF2;
        stats.errors++;
    }
    if (!DMA1_Channel2->CNDTR) {
        DMA1_Channel2->CCR = 0;
        if ((tc_pop(&txq, tx, now, 100) || tc_pop(&txlo, tx, now, 100))) {
            DMA1_Channel2->CPAR = (uint32_t)&USART3->DR;
            DMA1_Channel2->CMAR = (uint32_t)tx;
            DMA1_Channel2->CNDTR = 128;
            DMA1_Channel2->CCR = DMA_CCR_MINC | DMA_CCR_DIR | DMA_CCR_EN;
        }
    }
#elif TC_BACKEND == TC_I2C
    uint32_t p = lock();
    if (transaction_active && now - transaction_at > 50) {
        stats.errors++;
        transaction_at = 0;
        transaction_active = false;
        i2c_reset();
    }
    if (!tx_valid && !(I2C2->SR2 & I2C_SR2_BUSY)) {
        memset(tx, 0, 128);
        if ((tc_pop(&txq, tx, now, 100) || tc_pop(&txlo, tx, now, 100))) {
            tx_valid = 1;
            ready(true);
        }
    }
    unlock(p);
#else
    /* SPI TX is selected only at CS rising: immutable throughout the next transaction. */
#endif
}
bool tc_bus_receive(uint8_t *b, uint32_t now) {
    uint32_t p = lock();
    bool ok = tc_pop(&rxhi, b, now, 100) || tc_pop(&rxlo, b, now, 100);
    unlock(p);
    return ok;
}
void tc_bus_get_stats(tc_bus_stats *s) {
    uint32_t p = lock();
    *s = stats;
    unlock(p);
}

void tc_bus_cancel_pending(void){uint32_t p=lock();txq.head=txq.used=txlo.head=txlo.used=0;unlock(p);}
