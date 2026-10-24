#ifndef NOW_RADIO_H
#define NOW_RADIO_H
#include "now_protocol.h"
typedef struct {uint32_t submitted,link_ok,link_fail,rejected,expired,control_drop;} now_radio_stats_t;
void now_radio_stats(now_radio_stats_t *out);
bool now_radio_init(void);
bool now_radio_receive(now_request_t *q, uint32_t *received);
void now_radio_send(const uint8_t data[NOW_FRAME_SIZE]);
bool now_radio_receive_frame(uint8_t out[64],uint32_t *at);
bool now_radio_send_wave(const uint8_t data[64]);
void now_radio_service(void);
#endif
