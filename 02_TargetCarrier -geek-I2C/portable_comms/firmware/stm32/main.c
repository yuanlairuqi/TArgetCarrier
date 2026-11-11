#include "stm32f1xx.h"
#include "tc_bus.h"
#include "tc_board.h"
static volatile uint32_t ticks;
void SysTick_Handler(void) {
    ticks++;
}
static bool emit(void *ctx, const uint8_t *b, bool low, uint32_t now) {
    (void)ctx;
    (void)low;
    return tc_bus_send(b, now);
}
static void clock_init(void) {
    RCC->CR |= RCC_CR_HSEON;
    while (!(RCC->CR & RCC_CR_HSERDY)) {
    }
    FLASH->ACR = FLASH_ACR_PRFTBE | FLASH_ACR_LATENCY_2;
    RCC->CFGR = RCC_CFGR_PLLSRC | RCC_CFGR_PLLMULL9 | RCC_CFGR_PPRE1_DIV2;
    RCC->CR |= RCC_CR_PLLON;
    while (!(RCC->CR & RCC_CR_PLLRDY)) {
    }
    RCC->CFGR |= RCC_CFGR_SW_PLL;
    while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL) {
    }
    SystemCoreClock = 72000000;
    SysTick_Config(72000);
}
int main(void) {
    clock_init();
    tc_bus_init();
    tc_param params[] = {{1, TC_U32, 1, 25, 1, 1000}, {2, TC_F32, 1, 0x3f800000, 0, 100}};
    static tc_service service;
    static tc_wave_buffer waves;
    tc_service_init(&service, params, 2, emit, 0);
    uint32_t last_wave = 0, wseq = 0;
    for (;;) {
        uint32_t now = ticks;
        tc_bus_poll(now);
        uint8_t b[128];
        for (int i = 0; i < 4 && tc_bus_receive(b, now); i++)
            tc_service_receive(&service, b, now);
        tc_service_tick(&service, now);
        if (service.established && now - last_wave >= TC_WAVE_PERIOD_MS) {
            last_wave = now;
            uint16_t ids[] = {1, 2};
            uint32_t values[] = {
                now, params[0].bits}; /* demonstration values, not ADC/hardware measurement */
            tc_wave(tc_wave_buffer_emit, &waves, service.session, ++wseq, now, now * 1000,
                    TC_WAVE_PERIOD_MS * 1000, ids, 2, 1, TC_U32, values);
        }
        tc_wave_flush(&waves, emit, 0, now, 20);
        __WFI();
    }
}
