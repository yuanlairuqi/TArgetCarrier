#include "wl_stm.h"
#include "wl_codec.h"
#include "tc_bus.h"
#include "tc_board.h"
#include "stm32f1xx_hal.h"
#include <string.h>
static bool ready;static wl_codec codec;static uint8_t pending[WL_MAX];static unsigned used,offset;
static uint32_t enter(void){uint32_t p=__get_PRIMASK();__disable_irq();return p;}
bool wl_stm_init(void){if(ready)return true;wl_codec_init(&codec,false,WL_NODE,0);ready=tc_bus_init();return ready;}
bool wl_stm_bound(void){return codec.bound;}
void wl_stm_poll(void){if(ready)tc_bus_poll(HAL_GetTick());}
bool wl_stm_send(const uint8_t*d,unsigned n){if(!ready)return false;uint8_t b[128];uint32_t p=enter();bool ok=wl_encode(&codec,d,n,false,HAL_GetTick(),b)&&tc_bus_send(b,HAL_GetTick());__set_PRIMASK(p);return ok;}
unsigned wl_stm_read(uint8_t*out,unsigned cap){if(!ready||!out||!cap)return 0;wl_stm_poll();uint32_t p=enter();if(offset==used){uint8_t slot[128];used=offset=0;for(int i=0;i<8&&tc_bus_receive(slot,HAL_GetTick());i++){unsigned n;if(wl_decode(&codec,slot,pending,&n)){used=n;break;}}}unsigned n=cap;if(n>used-offset)n=used-offset;memcpy(out,pending+offset,n);offset+=n;__set_PRIMASK(p);return n;}
uint32_t wl_stm_errors(void){tc_bus_stats s;tc_bus_get_stats(&s);return s.errors+s.rx_drop+s.tx_drop+s.short_transaction+codec.rejected;}
