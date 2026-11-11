#ifndef WL_ESP_H
#define WL_ESP_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
bool wl_esp_init(void);
bool wl_esp_send(const uint8_t*d,size_t n,bool safe);
int wl_esp_read(uint8_t*out,size_t n);
#endif
