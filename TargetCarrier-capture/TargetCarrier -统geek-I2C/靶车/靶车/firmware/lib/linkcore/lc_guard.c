/**
 * @file    lc_guard.c
 * @brief   序号窗口 / ARM 授权 / 新鲜度 / 命令队列
 */

#include "linkcore.h"

#include <string.h>
#include <ctype.h>
#include <stdlib.h>

/* ================================================================== */
/* 序号窗口                                                           */
/* ================================================================== */

void lc_seq_gate_init(lc_seq_gate_t *g)
{
    if (g == NULL) {
        return;
    }
    memset(g, 0, sizeof(*g));
}

bool lc_seq_accept(lc_seq_gate_t *g, uint8_t seq)
{
    if (g == NULL) {
        return true;
    }
    if (!g->have) {
        g->have = true;
        g->last = seq;
        g->accepted++;
        return true;
    }

    int cmp = lc_seq_cmp8(seq, g->last);
    if (cmp == 0) {
        g->duplicate++;
        return false;
    }
    if (cmp < 0) {
        /* 比"已接受的最新序号"旧: 乱序/迟到/重传的旧帧, 一律不执行 */
        g->reorder++;
        return false;
    }
    g->last = seq;
    g->accepted++;
    return true;
}

/* ================================================================== */
/* ARM 授权 (STM32 侧)                                                */
/* ================================================================== */

void lc_arm_init(lc_arm_t *a)
{
    if (a == NULL) {
        return;
    }
    memset(a, 0, sizeof(*a));
}

void lc_arm_force_safe(lc_arm_t *a)
{
    if (a == NULL) {
        return;
    }
    /* 注意: 不清 last_token —— 这样"断联期间积压的旧使能帧"在恢复后
     * 仍然带着旧令牌, 会被当成重放拒绝掉。 */
    a->armed = false;
    a->safe_events++;
}

bool lc_arm_update(lc_arm_t *a, uint16_t flags)
{
    if (a == NULL) {
        return false;
    }

    /* 急停: 无论令牌如何, 立即撤销授权 (急停是电平, 解除后仍需新令牌) */
    if (flags & CTRL_FLAG_ESTOP) {
        if (a->armed) {
            a->disarm++;
        }
        a->armed = false;
        return false;
    }

    bool arm_bit = (flags & CTRL_FLAG_ARM) != 0u;
    bool enable_bit = (flags & CTRL_FLAG_ENABLE) != 0u;
    uint8_t token = proto_flags_token(flags);

    /* 启动门: 复位后必须先看到一帧"未使能且未授权"的主机帧, 才允许后续授权。
     * 原因: STM32 复位只需几十毫秒, 主机的 300ms 失联判定可能还没触发;
     * 此时若主机仍在发复位前的 ENABLE|ARM|旧令牌, last_token=0 会把它当成
     * 新授权, 电机就会在复位后自动恢复运动。 */
    if (!a->boot_disarm_seen) {
        if (!enable_bit && !arm_bit) {
            a->boot_disarm_seen = true;
        } else {
            a->arm_reject++;
            return false;
        }
    }

    if (!arm_bit || !enable_bit) {
        /* 没有授权脉冲/没有使能位: 撤销授权 (主机主动失能的路径) */
        if (a->armed) {
            a->disarm++;
        }
        a->armed = false;
        return false;
    }

    if (token == CTRL_ARM_TOKEN_NONE) {
        a->arm_reject++;
        return false;
    }

    if (a->armed) {
        /* 已授权: 同一令牌的后续帧只是保持; 换令牌则视为一次新的明确授权 */
        if (token != a->last_token) {
            a->last_token = token;
            a->arm_ok++;
        }
        return true;
    }

    /* 未授权 + 收到完整授权帧: 仅当令牌不是"上次已被接受过的那个"才接受。
     * 这挡住两类危险帧:
     *   1) 断联/急停/故障前就在管道里、恢复后才到的旧 ENABLE 帧;
     *   2) 主机没收到故障通知、继续无条件重发的同一个旧授权。 */
    if (token == a->last_token) {
        a->arm_reject++;
        return false;
    }

    a->last_token = token;
    a->armed = true;
    a->arm_ok++;
    return true;
}

/* ================================================================== */
/* ARM 授权产生侧 (ESP32 主机)                                        */
/* ================================================================== */

void lc_arm_tx_init(lc_arm_tx_t *t, uint8_t seed)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
    /* 令牌 0 保留给"未授权", 因此把 seed 规约到 1..255 */
    t->token = (uint8_t)(seed == 0u ? 1u : seed);
    t->active = false;
    t->issued = 0;
}

uint8_t lc_arm_tx_next(lc_arm_tx_t *t)
{
    if (t == NULL) {
        return CTRL_ARM_TOKEN_NONE;
    }
    uint8_t next = (uint8_t)(t->token + 1u);
    if (next == CTRL_ARM_TOKEN_NONE) {
        next = 1u;
    }
    t->token = next;
    t->active = true;
    t->issued++;
    return t->token;
}

void lc_arm_tx_clear(lc_arm_tx_t *t)
{
    if (t == NULL) {
        return;
    }
    t->active = false;
}

/* ================================================================== */
/* 新鲜度                                                             */
/* ================================================================== */

void lc_fresh_init(lc_fresh_t *f)
{
    if (f == NULL) {
        return;
    }
    memset(f, 0, sizeof(*f));
}

void lc_fresh_kick(lc_fresh_t *f, uint32_t now_ms)
{
    if (f == NULL) {
        return;
    }
    f->last_ms = now_ms;
    f->have = true;
    f->updates++;
}

bool lc_fresh_ok(const lc_fresh_t *f, uint32_t now_ms, uint32_t timeout_ms)
{
    if (f == NULL || !f->have) {
        return false;
    }
    return lc_elapsed_ms(now_ms, f->last_ms) <= timeout_ms;
}

uint32_t lc_fresh_age(const lc_fresh_t *f, uint32_t now_ms, bool *have)
{
    if (f == NULL || !f->have) {
        if (have != NULL) {
            *have = false;
        }
        return 0xFFFFFFFFu;
    }
    if (have != NULL) {
        *have = true;
    }
    return lc_elapsed_ms(now_ms, f->last_ms);
}

/* ================================================================== */
/* 控制命令队列                                                       */
/* ================================================================== */

void lc_cmdq_init(lc_cmdq_t *q)
{
    if (q == NULL) {
        return;
    }
    memset(q, 0, sizeof(*q));
}

static void cmdq_remove_at(lc_cmdq_t *q, uint8_t idx)
{
    if (idx >= q->n) {
        return;
    }
    for (uint8_t i = (uint8_t)(idx + 1u); i < q->n; i++) {
        q->q[i - 1u] = q->q[i];
    }
    q->n--;
}

bool lc_cmdq_push(lc_cmdq_t *q, const lc_cmd_t *cmd)
{
    if (q == NULL || cmd == NULL) {
        return false;
    }

    if (q->n < LC_CMD_QMAX) {
        q->q[q->n++] = *cmd;
        q->push_ok++;
        return true;
    }

    if (cmd->safety) {
        /* 队列满: 安全命令腾掉一条运动命令; 全是安全命令时丢掉最旧的安全命令
         * (新的安全命令比旧的安全命令更接近当前意图, 例如先使能后失能)。 */
        for (uint8_t i = 0; i < q->n; i++) {
            if (!q->q[i].safety) {
                cmdq_remove_at(q, i);
                q->push_drop_motion++;
                q->q[q->n++] = *cmd;
                q->push_ok++;
                return true;
            }
        }
        cmdq_remove_at(q, 0);
        q->push_drop_safety++;
        q->q[q->n++] = *cmd;
        q->push_ok++;
        return true;
    }

    q->push_drop_motion++;
    return false;
}

uint32_t lc_cmdq_purge_motion(lc_cmdq_t *q)
{
    if (q == NULL) {
        return 0;
    }
    uint32_t removed = 0;
    uint8_t i = 0;
    while (i < q->n) {
        if (!q->q[i].safety) {
            cmdq_remove_at(q, i);
            removed++;
        } else {
            i++;
        }
    }
    q->purged_motion += removed;
    return removed;
}

void lc_cmdq_reset(lc_cmdq_t *q)
{
    if (q == NULL) {
        return;
    }
    q->n = 0;
}

bool lc_cmdq_pop(lc_cmdq_t *q, uint32_t now_ms, uint32_t max_age_ms,
                 lc_seq_gate_t *seq_gate, lc_cmd_t *out)
{
    if (q == NULL || out == NULL) {
        return false;
    }

    for (;;) {
        if (q->n == 0u) {
            return false;
        }

        /* 1) 先找安全命令 (最高优先级) */
        uint8_t pick = 0xFFu;
        for (uint8_t i = 0; i < q->n; i++) {
            if (q->q[i].safety) {
                pick = i;
                break;
            }
        }
        /* 2) 没有安全命令再取最旧的运动命令 (FIFO 保持时间顺序) */
        if (pick == 0xFFu) {
            pick = 0u;
        }

        lc_cmd_t cmd = q->q[pick];
        cmdq_remove_at(q, pick);

        /* 过期命令绝不能再当成"新命令"执行 */
        if (max_age_ms != 0u && lc_age_exceeds(now_ms, cmd.rx_ms, max_age_ms)) {
            q->expired++;
            continue;
        }

        if (seq_gate != NULL && !lc_seq_accept(seq_gate, cmd.seq)) {
            /* 重复/乱序: 丢弃, 不执行; 继续看下一条 */
            continue;
        }

        *out = cmd;
        return true;
    }
}

lc_stop_command_t lc_stop_command(const char *line)
{
    if (!line) return LC_STOP_NONE;
    while (isspace((unsigned char)*line)) ++line;
    char cmd = *line++;
    if (!cmd) return LC_STOP_NONE;
    if (cmd == 'x' || cmd == 'C' || cmd == 'R') {
        while (isspace((unsigned char)*line)) ++line;
        if (*line) return LC_STOP_NONE;
        return cmd == 'x' ? LC_STOP_ESTOP : cmd == 'C' ? LC_STOP_DISABLE : LC_STOP_RELEASE;
    }
    if (cmd != 'V' && cmd != 'U' && cmd != 'W') return LC_STOP_NONE;
    char *end;
    float value = strtof(line, &end);
    if (end == line || value != 0.0f) return LC_STOP_NONE;
    while (isspace((unsigned char)*end)) ++end;
    if (*end) return LC_STOP_NONE;
    return cmd == 'V' ? LC_STOP_YAW : cmd == 'U' ? LC_STOP_PITCH : LC_STOP_CRUISE;
}
