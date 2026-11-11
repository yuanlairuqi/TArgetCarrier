#ifndef WL_CODEC_H
#define WL_CODEC_H
#include "tc.h"
#ifdef __cplusplus
extern "C" {
#endif
#define WL_MAX 32
typedef struct {bool master,bound,seen;uint8_t node;uint32_t session,tx_seq,rx_seq,echo,last_echo,rejected,duplicate;} wl_codec;
void wl_codec_init(wl_codec*c,bool master,uint8_t node,uint32_t session);
bool wl_encode(wl_codec*c,const uint8_t*data,unsigned n,bool safe,uint32_t now,uint8_t*out);
bool wl_decode(wl_codec*c,const uint8_t*slot,uint8_t*out,unsigned*len);
#ifdef __cplusplus
}
#endif
#endif
