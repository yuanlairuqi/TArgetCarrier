#pragma once
#include "now_protocol.h"
#ifndef MASTER_TRANSPORT_MS
#define MASTER_TRANSPORT_MS 600U
#endif
typedef struct {
    uint32_t boot,epoch,session,blocked,last_seq,last_pc,last_radio,losses;
    uint32_t retired_boot[4],retired_epoch[4];unsigned retired_at;
    bool challenge,queried,active,unlocked;
} master_guard_t;
void master_guard_loss(master_guard_t *g);
void master_guard_query(master_guard_t *g,uint32_t now);
bool master_guard_status(master_guard_t *g,uint32_t boot,uint32_t epoch,uint32_t now);
bool master_guard_command(master_guard_t *g,const now_request_t *q,uint32_t now);
bool master_guard_expired(const master_guard_t *g,bool usb,uint32_t now);
