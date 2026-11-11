#ifndef WL_STM_H
#define WL_STM_H
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
bool wl_stm_init(void);bool wl_stm_bound(void);
void wl_stm_poll(void);bool wl_stm_send(const uint8_t*d,unsigned n);
unsigned wl_stm_read(uint8_t*out,unsigned cap);
uint32_t wl_stm_errors(void);void wl_spi_cs_rising(void);
#ifdef __cplusplus
}
#endif
#endif
