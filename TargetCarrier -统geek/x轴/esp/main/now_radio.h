#ifndef NOW_RADIO_H
#define NOW_RADIO_H
#include "now_protocol.h"
bool now_radio_init(void);
bool now_radio_receive(now_request_t *q, uint32_t *received);
void now_radio_send(const uint8_t data[NOW_FRAME_SIZE]);
#endif
