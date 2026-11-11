#ifndef TARGET_BLE_GUARD_H
#define TARGET_BLE_GUARD_H
#include "linkcore.h"
typedef struct {
    bool connected, have_pc, disarm_seen, failsafe, active;
    uint8_t token, blocked_token;
    uint32_t last_pc, accepted, rejected;
    lc_seq_gate_t seq;
} tb_guard_t;
void tb_guard_init(tb_guard_t *g);
void tb_guard_connection(tb_guard_t *g,bool connected);
bool tb_frame_valid(const uint8_t *f,size_t size);
bool tb_guard_accept(tb_guard_t *g,const uint8_t f[17],uint32_t now,bool stm_fresh);
bool tb_guard_tick(tb_guard_t *g,uint32_t now,bool stm_fresh);
#endif
