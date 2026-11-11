#include "wl_esp.h"
#include "wl_codec.h"
#include "tc_bus.h"
#include "tc_board.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"
#include "esp_random.h"
#include <string.h>
static StaticSemaphore_t storage;static SemaphoreHandle_t mutex;static bool ready;static wl_codec codec;static uint8_t pending[WL_MAX];static unsigned used,offset;
static uint32_t ms(void){return (uint32_t)(esp_timer_get_time()/1000);}
bool wl_esp_init(void){if(mutex)return ready;mutex=xSemaphoreCreateMutexStatic(&storage);if(!mutex)return false;wl_codec_init(&codec,true,WL_NODE,esp_random());ready=tc_bus_init();return ready;}
bool wl_esp_send(const uint8_t*d,size_t n,bool safe){if(!ready)return false;xSemaphoreTake(mutex,portMAX_DELAY);uint8_t slot[128];bool ok=wl_encode(&codec,d,(unsigned)n,safe,ms(),slot);if(ok){if(safe)tc_bus_cancel_pending();ok=tc_bus_send(slot,ms());tc_bus_poll(ms());}xSemaphoreGive(mutex);return ok;}
int wl_esp_read(uint8_t*out,size_t cap){if(!ready||!out||!cap)return 0;xSemaphoreTake(mutex,portMAX_DELAY);tc_bus_poll(ms());if(offset==used){uint8_t slot[128];used=offset=0;for(int i=0;i<8&&tc_bus_receive(slot,ms());i++){unsigned n;if(wl_decode(&codec,slot,pending,&n)){used=n;break;}}}unsigned n=(unsigned)cap;if(n>used-offset)n=used-offset;memcpy(out,pending+offset,n);offset+=n;xSemaphoreGive(mutex);return (int)n;}
