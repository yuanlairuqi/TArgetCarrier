#ifndef NOW_RADIO_H
#define NOW_RADIO_H
#include "now_protocol.h"
#ifndef NOW_RADIO_CALLBACK_MS
#define NOW_RADIO_CALLBACK_MS 250U
#endif
#ifndef NOW_RADIO_RECOVERY_MS
#define NOW_RADIO_RECOVERY_MS 1000U
#endif
#ifndef NOW_WAVE_GAP_MS
#define NOW_WAVE_GAP_MS 20U
#endif
typedef struct {uint32_t submitted,link_ok,link_fail,rejected,expired,control_drop,rx_drop,wave_drop,rx_high,tx_high,callback_timeout,recoveries,auth_fail;} now_radio_stats_t;
void now_radio_stats(now_radio_stats_t *out);
bool now_radio_init(void);
bool now_radio_receive(now_request_t *q, uint32_t *received);
void now_radio_send(const uint8_t data[NOW_FRAME_SIZE]);
bool now_radio_receive_frame(uint8_t out[64],uint32_t *at);
bool now_radio_send_wave(const uint8_t data[64]);
void now_radio_service(void);
void now_radio_flush(void);
#endif
