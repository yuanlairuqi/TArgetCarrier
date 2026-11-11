#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define USB_FRAME_SIZE 72
typedef struct {uint8_t bytes[72];size_t used;} usb_parser_t;
uint32_t wire_get32(const uint8_t *p);
void wire_put32(uint8_t *p,uint32_t v);
bool usb_feed(usb_parser_t *p,uint8_t b,uint8_t body[64],uint8_t *kind);
void usb_pack(uint8_t kind,const uint8_t body[64],uint8_t out[72]);
bool master_output_confirmed(const uint8_t status[64],const uint8_t command[64]);
