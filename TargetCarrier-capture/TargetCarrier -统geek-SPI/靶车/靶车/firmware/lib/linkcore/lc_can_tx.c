/**
 * @file    lc_can_tx.c
 * @brief   CAN 发送协调策略 —— 安全优先、运动帧只保留最新、停止作废旧启动
 *
 *  典型用法 (STM32 控制任务, 1 kHz):
 *      // 安全命令 (边沿触发, 由命令处理/安全状态机产生)
 *      lc_can_tx_request(&tx, &disable_frame);
 *      // 每拍的运动帧 (覆盖上一拍)
 *      lc_can_tx_request(&tx, &mit_frame);
 *      // 本拍尽量把邮箱填满, 但不阻塞
 *      for (int i = 0; i < 3; i++) {
 *          const lc_can_frame_t *f = lc_can_tx_peek(&tx);
 *          if (!f) break;
 *          bool written = can_drv_send(f->id, f->data, f->len);
 *          lc_can_tx_pop(&tx, written);
 *          if (!written) break;      // 邮箱满: 留给下一拍, 不忙等
 *      }
 *      // 邮箱完成 (TSR.TMEx) 由 can 任务上报:
 *      lc_can_tx_report_bus(&tx, true);
 */

#include "linkcore.h"

#include <string.h>

void lc_can_tx_init(lc_can_tx_t *t)
{
    if (t == NULL) {
        return;
    }
    memset(t, 0, sizeof(*t));
}

static void safety_clear(lc_can_tx_t *t, lc_can_safety_t tag)
{
    t->safety_pending[tag] = false;
}

bool lc_can_tx_request(lc_can_tx_t *t, const lc_can_frame_t *f)
{
    if (t == NULL || f == NULL) {
        return false;
    }

    if (f->prio == (uint8_t)LC_CAN_PRIO_SAFETY) {
        if (f->tag >= (uint8_t)LC_CAN_SAFETY_N) {
            return false;
        }
        lc_can_safety_t tag = (lc_can_safety_t)f->tag;

        /* 停止屏障: 请求失能时, 队列里的旧使能和旧运动目标立刻作废,
         * 否则"停止后执行旧的启动命令"会在这里发生。 */
        if (tag == LC_CAN_SAFETY_DISABLE) {
            safety_clear(t, LC_CAN_SAFETY_ENABLE);
            t->have_motion = false;
        }

        if (t->safety_pending[tag]) {
            t->safety_dropped++;   /* 同类命令被更新的覆盖 (只保留最新) */
        }
        t->safety[tag] = *f;
        t->safety_pending[tag] = true;
        t->safety_seq[tag] = ++t->safety_clock;
        return true;
    }

    if (f->prio == (uint8_t)LC_CAN_PRIO_MOTION) {
        if (t->have_motion) {
            t->motion_coalesced++;
        }
        t->motion = *f;
        t->have_motion = true;
        return true;
    }

    /* 参数帧: 低优先级 FIFO, 满了明确丢并计数 (调用方可据此报失败) */
    if (t->n_param < LC_CAN_PARAM_QMAX) {
        t->param[t->n_param++] = *f;
        return true;
    }
    t->param_dropped++;
    return false;
}

bool lc_can_tx_pending(const lc_can_tx_t *t)
{
    if (t == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < (uint8_t)LC_CAN_SAFETY_N; i++) {
        if (t->safety_pending[i]) {
            return true;
        }
    }
    return t->have_motion || (t->n_param > 0u);
}

const lc_can_frame_t *lc_can_tx_peek(const lc_can_tx_t *t)
{
    if (t == NULL) {
        return NULL;
    }
    /* 1. 安全命令: 固定优先级 DISABLE > ENABLE > CLEAR > ZERO */
    for (uint8_t i = 0; i < (uint8_t)LC_CAN_SAFETY_N; i++) {
        if (t->safety_pending[i]) {
            return &t->safety[i];
        }
    }
    /* 2. 最新运动帧 */
    if (t->have_motion) {
        return &t->motion;
    }
    /* 3. 参数帧 */
    if (t->n_param > 0u) {
        return &t->param[0];
    }
    return NULL;
}

void lc_can_tx_pop(lc_can_tx_t *t, bool written)
{
    if (t == NULL) {
        return;
    }

    if (!written) {
        /* 邮箱满/总线忙: 帧留在原位, 下一拍再试 (不阻塞、不忙等) */
        t->submit_fail++;
        return;
    }

    const lc_can_frame_t *f = lc_can_tx_peek(t);
    if (f == NULL) {
        return;
    }

    if (f->prio == (uint8_t)LC_CAN_PRIO_SAFETY) {
        lc_can_safety_t tag = (lc_can_safety_t)f->tag;
        if (tag < LC_CAN_SAFETY_N) {
            t->safety_pending[tag] = false;
        }
        /* Requesting DISABLE already purged older ENABLE requests. Do not
         * erase a newer explicit ENABLE posted after that stop barrier. */
    } else if (f->prio == (uint8_t)LC_CAN_PRIO_MOTION) {
        t->have_motion = false;
    } else {
        if (t->n_param > 0u) {
            for (uint8_t i = 1; i < t->n_param; i++) {
                t->param[i - 1u] = t->param[i];
            }
            t->n_param--;
        }
    }
    t->submitted++;
}

void lc_can_tx_report_bus(lc_can_tx_t *t, bool ok)
{
    if (t == NULL) {
        return;
    }
    if (ok) {
        t->bus_ok++;
    } else {
        t->bus_fail++;
    }
}

void lc_can_tx_abort(lc_can_tx_t *t)
{
    if (t == NULL) {
        return;
    }
    /* CAN 重新初始化: 旧运动/参数帧的"目标"已在恢复前失效, 不能恢复后再发;
     * 安全命令 (尤其失能) 继续保留, 恢复后优先补发。 */
    t->have_motion = false;
    t->n_param = 0;
    t->recoveries++;
}

void lc_can_tx_clear_motion(lc_can_tx_t *t)
{
    if (t == NULL) {
        return;
    }
    t->have_motion = false;
}
