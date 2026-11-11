#pragma once
#include "now_protocol.h"
typedef struct {bool active;uint32_t session,seq,last;} tn_session_t;
uint8_t tn_accept(tn_session_t *,const now_request_t *,uint32_t boot,uint32_t epoch,uint32_t at,uint32_t now);
