/**
 * @file    app_main.c
 * @brief   靶车 STM32F103C8T6 控制器 —— FreeRTOS 版主逻辑
 *
 *  任务与优先级 (见 app_tasks.h):
 *    ctrl(5) / link(4) / can(4) / status(3) / ui(1)
 *
 *  安全设计 (本轮重点):
 *    1. 三层失联保护保留: 桥接 300ms 注入失能帧 + STM32 自身 300ms 超时 +
 *       增益斜坡。STM32 的超时从旧版的"1s 宽限"收紧为 300ms, 与桥接/文档统一。
 *    2. 新增 ARM 授权: 只有"用户明确使能"产生的令牌才能让电机闭环;
 *       断联/急停/故障/CAN 恢复/复位后, 旧令牌一律拒绝 (lc_arm_t)。
 *    3. 停止屏障: 收到失能/急停时立刻撤销授权、清掉 CAN 发送表里的旧运动目标,
 *       不再"每次循环连发 50 帧失能" (那种写法无法保证实际发出数量)。
 *    4. CAN 发送单一所有者 (ctrl 任务) + 互斥量协调恢复, 中断绝不发送。
 *    5. 命令队列带 100ms 有效期 + 8bit 序号窗口, 积压/旧命令不会被当成新命令。
 *    6. 电机反馈 200ms 超时 -> 视为故障并失能 (不把旧反馈当成当前状态)。
 */

#include "app_main.h"
#include "app_tasks.h"
#include "config.h"
#include "gimbal_motion.h"
#include "protocol.h"
#include "linkcore.h"
#include "link_uart.h"
#include "can_drv.h"
#include "dm_motor.h"
#include "timer_drv.h"
#include "display.h"
#include "debug_uart.h"

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"

#include "stm32f1xx.h"   /* 仅用于 NVIC_SystemReset (控制节拍丢失时的兜底复位) */

#include <math.h>
#include <string.h>

/* ================================================================== */
/* 常量                                                               */
/* ================================================================== */

#define DEG2RAD  ((float)M_PI / 180.0f)
#define RAD2DEG  (180.0f / (float)M_PI)

#define GAIN_RAMP_STEPS   500u    /* 500 步 @1kHz = 500ms 使能斜坡 */
#define GAIN_RAMP_DOWN    5u      /* 失能斜坡: 每 ms 减 5 步 -> 100ms 收敛 */

#define CTRL_TICK_HZ      1000u

/* ================================================================== */
/* 静态分配的任务 / 队列 / 互斥量                                      */
/* ================================================================== */

static StaticTask_t s_ctrl_tcb;
static StackType_t  s_ctrl_stack[APP_STACK_CTRL_WORDS];
static TaskHandle_t s_ctrl_task = NULL;

static StaticTask_t s_link_tcb;
static StackType_t  s_link_stack[APP_STACK_LINK_WORDS];
static TaskHandle_t s_link_task_h = NULL;

static StaticTask_t s_can_tcb;
static StackType_t  s_can_stack[APP_STACK_CAN_WORDS];
static TaskHandle_t s_can_task_h = NULL;

static StaticTask_t s_status_tcb;
static StackType_t  s_status_stack[APP_STACK_STATUS_WORDS];
static TaskHandle_t s_status_task_h = NULL;

static StaticTask_t s_ui_tcb;
static StackType_t  s_ui_stack[APP_STACK_UI_WORDS];
static TaskHandle_t s_ui_task_h = NULL;

typedef struct {
    float kp;
    float kd;
} gain_msg_t;

static StaticQueue_t s_gain_qbuf;
static uint8_t       s_gain_qstore[2u * sizeof(gain_msg_t)];
static QueueHandle_t s_gain_q = NULL;

static StaticQueue_t s_param_qbuf;
static uint8_t       s_param_qstore[2u * sizeof(app_param_t)];
static QueueHandle_t s_param_q = NULL;

static StaticQueue_t s_reply_qbuf;
static uint8_t       s_reply_qstore[2u * sizeof(app_param_reply_t)];
static QueueHandle_t s_reply_q = NULL;

/* 命令队列 + 互斥量: link 任务写, ctrl 任务读, 逻辑在 lib/linkcore */
static lc_cmdq_t         s_cmdq;
static StaticSemaphore_t s_cmdq_mtx_buf;
static SemaphoreHandle_t s_cmdq_mtx = NULL;

/* ================================================================== */
/* 共享运行时状态                                                     */
/* ================================================================== */

/* 电机反馈 (can 任务写, ctrl/status/ui 读; 临界区保护) */
typedef struct {
    float    pos_rad;
    float    vel_rad_s;
    float    torque_nm;
    uint8_t  temp_mos;
    uint8_t  err;
    bool     valid;
    uint32_t ms;
} fb_state_t;

static fb_state_t g_fb;

/* 一致快照 (ctrl 任务整体更新, status/ui 整体拷贝) */
static app_rt_state_t g_snap;

/* 跨任务事件计数 (无锁 32 位标量; 只做"是否变化"判断) */
static volatile uint32_t g_failsafe_events = 0;   /* link: 收到桥接 FAILSAFE 帧 */
static volatile uint32_t g_can_recover_events = 0;/* can: 完成一次总线恢复 */
static volatile uint32_t g_last_ctrl_ms = 0;      /* link: 最近有效控制帧时刻 */
static volatile bool     g_have_ctrl = false;
static volatile bool     g_recover_req = false;   /* ctrl -> can: 请求恢复 */
static volatile uint32_t g_ctrl_ticks = 0;        /* TIM2 ISR 计数 */
static volatile uint32_t g_link_drop_cmds = 0;    /* link: 队列满/解析丢弃 */

/* ctrl 任务私有状态 */
static uint16_t s_target_flags = 0;
static float    s_target_pos_rad = 0.0f;
static float    s_target_vel_rad_s = 0.0f;
static gm_motion_t s_motion;
static uint32_t s_motion_at;
static float    s_target_kp = MOTOR_KP_DEFAULT;
static float    s_target_kd = MOTOR_KD_DEFAULT;
static float    s_kp_now = 0.0f;
static float    s_kd_now = 0.0f;
static uint16_t s_ramp = 0;
static bool     s_enabled = false;
static bool     s_estop = false;
static bool     s_fault = false;
static bool     s_link_lost = true;
static uint32_t s_gains_rx = 0;
static uint32_t s_arm_reject_seen = 0;
static uint32_t s_fail_streak = 0;
static uint32_t s_prev_submit_fail = 0;
static uint32_t s_fb_grace_until_ms = APP_FB_GRACE_MS;
static uint32_t s_fault_mask_until_ms = 0;   /* 清错后忽略旧故障码的窗口 */
static uint32_t s_seen_failsafe = 0;
static uint32_t s_seen_recover = 0;
static uint32_t s_disarm_ms = 0;
static bool     s_need_disable_burst = false;
static uint32_t s_disable_sent = 0;

static lc_arm_t   s_arm;
static lc_can_tx_t s_can_tx;
static bool s_abort_mailboxes = false;

/* status 任务私有 */
static uint8_t s_tx_seq = 0;

/* ================================================================== */
/* 小工具                                                             */
/* ================================================================== */

static inline uint32_t now_ms(void)
{
    /* 1 tick = 1 ms (configTICK_RATE_HZ=1000) */
    return (uint32_t)xTaskGetTickCount();
}

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static void snap_load(app_rt_state_t *out)
{
    taskENTER_CRITICAL();
    *out = g_snap;
    taskEXIT_CRITICAL();
}

static void fb_read(fb_state_t *out)
{
    taskENTER_CRITICAL();
    *out = g_fb;
    taskEXIT_CRITICAL();
}

static void fb_update(const dm_feedback_t *fb)
{
    taskENTER_CRITICAL();
    g_fb.pos_rad   = fb->pos_rad;
    g_fb.vel_rad_s = fb->vel_rad_s;
    g_fb.torque_nm = fb->torque_nm;
    g_fb.temp_mos  = fb->temp_mos;
    g_fb.err       = fb->err;
    g_fb.valid     = true;
    g_fb.ms        = now_ms();
    taskEXIT_CRITICAL();
}

/* ================================================================== */
/* CAN 发送辅助 (只在 ctrl 任务里调用)                                 */
/* ================================================================== */

static void can_request_safety(lc_can_safety_t tag, uint32_t id,
                               const uint8_t data[8])
{
    lc_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id   = id;
    f.len  = 8u;
    f.prio = (uint8_t)LC_CAN_PRIO_SAFETY;
    f.tag  = (uint8_t)tag;
    memcpy(f.data, data, 8u);
    (void)lc_can_tx_request(&s_can_tx, &f);
}

static void can_submit_disable(void)
{
    s_abort_mailboxes = true; /* Also cancel old ENABLE/motion already in hardware. */
    uint8_t d[8];
    uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_DISABLE, d);
    can_request_safety(LC_CAN_SAFETY_DISABLE, id, d);
    s_disarm_ms = now_ms();
    s_need_disable_burst = true;
    s_disable_sent = 1;
}

static void can_submit_enable(void)
{
    s_need_disable_burst = false;
    uint8_t d[8];
    uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_ENABLE, d);
    can_request_safety(LC_CAN_SAFETY_ENABLE, id, d);
}

static void can_submit_clear_err(void)
{
    uint8_t d[8];
    uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_CLEAR_ERR, d);
    can_request_safety(LC_CAN_SAFETY_CLEAR, id, d);
}

static void can_submit_save_zero(void)
{
    uint8_t d[8];
    uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_SAVE_ZERO, d);
    can_request_safety(LC_CAN_SAFETY_ZERO, id, d);
}

/* 提交参数读写请求 (低优先级; 队列满明确失败并回包) */
static void can_submit_param(const app_param_t *req)
{
    /* Local read-only capability: old firmware cannot acknowledge this magic.
     * It travels through the existing PARAM reply path, never onto CAN. */
    if (req->rid == PROTO_CAPABILITY_RID) {
        app_param_reply_t r = { req->rid, req->op == PROTO_PARAM_OP_READ, {0,0,0,0} };
        uint32_t value = PROTO_CAPABILITY_VALUE;
        if (r.ok) memcpy(r.value, &value, sizeof(value));
        (void)xQueueSend(s_reply_q, &r, 0);
        return;
    }
    uint8_t d[8] = { 0 };
    d[0] = (uint8_t)(MOTOR_CAN_ID & 0xFFu);
    d[1] = (uint8_t)((MOTOR_CAN_ID >> 8) & 0xFFu);

    switch (req->op) {
    case PROTO_PARAM_OP_READ:
        d[2] = 0x33;
        d[3] = req->rid;
        break;
    case PROTO_PARAM_OP_WRITE_U:
    case PROTO_PARAM_OP_WRITE_F:
        d[2] = 0x55;
        d[3] = req->rid;
        memcpy(&d[4], req->value, 4);
        break;
    case PROTO_PARAM_OP_SAVE:
        d[2] = 0xAA;
        d[3] = 0x01;
        break;
    default:
        {
            app_param_reply_t r = { req->rid, 0u, {0,0,0,0} };
            (void)xQueueSend(s_reply_q, &r, 0);
        }
        return;
    }

    lc_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id   = 0x7FFu;
    f.len  = 8u;
    f.prio = (uint8_t)LC_CAN_PRIO_PARAM;
    memcpy(f.data, d, 8u);

    if (!lc_can_tx_request(&s_can_tx, &f)) {
        app_param_reply_t r = { req->rid, 0u, {0,0,0,0} };
        (void)xQueueSend(s_reply_q, &r, 0);
    }
}

/* ================================================================== */
/* 控制节拍: TIM2 中断 -> 通知 ctrl 任务                              */
/* ================================================================== */

static void ctrl_tick_isr(void)
{
    BaseType_t woken = pdFALSE;
    g_ctrl_ticks++;
    if (s_ctrl_task != NULL) {
        /* 这里只做"发通知", 不做任何控制计算: 中断耗时必须 <= 几微秒,
         * 控制计算放 ctrl 任务, 由中断退出时立即抢占调度 (见文档抖动说明)。 */
        (void)xTaskNotifyFromISR(s_ctrl_task, APP_NOTIFY_CTRL_TICK, eSetBits, &woken);
    }
    portYIELD_FROM_ISR(woken);
}

/* ================================================================== */
/* 命令应用 (ctrl 任务)                                                */
/* ================================================================== */

static void apply_command(const lc_cmd_t *cmd, uint32_t now)
{
    ctrl_target_t t;
    ctrl_payload_t pl;
    memset(&pl, 0, sizeof(pl));
    pl.flags       = cmd->flags;
    pl.mode        = cmd->mode;
    pl.yaw_q       = cmd->yaw_q;
    pl.pitch_q     = cmd->pitch_q;
    pl.yawrate_q   = cmd->yawrate_q;
    pl.pitchrate_q = cmd->pitchrate_q;
    proto_payload_to_target(&pl, &t);

    /* Orientation only; continuous speed ignores position entirely. */
    float yaw_deg = t.yaw_deg;
    float pitch_deg = clampf(t.pitch_deg, GIMBAL_PITCH_MIN_DEG, GIMBAL_PITCH_MAX_DEG);
    (void)pitch_deg;   /* 单轴云台: 俯仰暂不驱动 */

    s_target_pos_rad   = clampf(yaw_deg * DEG2RAD, -MOTOR_PMAX, MOTOR_PMAX);
    s_target_vel_rad_s = clampf(t.yaw_rate_dps * DEG2RAD, -MOTOR_VMAX, MOTOR_VMAX);

    /* ---- 急停 ---- */
    if (cmd->flags & CTRL_FLAG_ESTOP) {
        if (!s_estop) {
            s_estop = true;
            lc_arm_force_safe(&s_arm);
            can_submit_disable();
            lc_can_tx_clear_motion(&s_can_tx);
        }
        s_target_flags = (uint16_t)(cmd->flags & (uint16_t)~CTRL_FLAG_ENABLE);
        return;
    }
    s_estop = false;

    /* ---- 一次性命令 ---- */
    if (cmd->flags & CTRL_FLAG_CLEAR_ERR) {
        can_submit_clear_err();
        s_fault = false;
        s_fail_streak = 0;
        s_fb_grace_until_ms = now + APP_FB_GRACE_MS;
        /* 电机下一帧反馈里可能还带着旧的故障码, 500ms 内不据此重新置故障,
         * 否则"清错"永远清不掉 (旧实现也有这个问题)。 */
        s_fault_mask_until_ms = now + 500u;
        s_arm_reject_seen = 0;
    }
    if (cmd->flags & CTRL_FLAG_SET_ZERO) {
        can_submit_save_zero();
    }

    /* ---- 授权状态机 ---- */
    bool was_armed = s_arm.armed;
    bool now_armed = lc_arm_update(&s_arm, cmd->flags);
    if (now_armed && !was_armed) {
        can_submit_enable();
    } else if (!now_armed && was_armed) {
        can_submit_disable();
        lc_can_tx_clear_motion(&s_can_tx);
    }
    if (s_arm.arm_reject > s_arm_reject_seen) {
        s_arm_reject_seen = s_arm.arm_reject;
    }

    s_target_flags = cmd->flags;
}

/* ================================================================== */
/* 控制任务                                                           */
/* ================================================================== */

static bool fb_is_fresh(const fb_state_t *fb, uint32_t now, uint32_t *age_out)
{
    if (!fb->valid) {
        if (age_out != NULL) {
            *age_out = 0xFFFFFFFFu;
        }
        return false;
    }
    uint32_t age = lc_elapsed_ms(now, fb->ms);
    if (age_out != NULL) {
        *age_out = age;
    }
    return age <= APP_FB_TIMEOUT_MS;
}

static void ctrl_service_can(uint32_t now)
{
    if (!can_drv_is_recovering() && can_drv_lock(0)) {
        can_tx_done_t done = can_drv_poll_tx();
        for (uint8_t i = 0; i < done.ok; ++i) lc_can_tx_report_bus(&s_can_tx, true);
        for (uint8_t i = 0; i < done.failed; ++i) lc_can_tx_report_bus(&s_can_tx, false);
        if (done.ok) s_fail_streak = 0;
        s_fail_streak += done.failed;
        if (s_abort_mailboxes) {
            can_drv_abort_all();
            s_abort_mailboxes = false;
        }
        for (uint8_t i = 0; i < 3u; ++i) {
            const lc_can_frame_t *f = lc_can_tx_peek(&s_can_tx);
            if (f == NULL) break;
            bool ok = can_drv_send(f->id, f->data, f->len);
            lc_can_tx_pop(&s_can_tx, ok);
            if (!ok) break;
        }
        can_drv_unlock();
    }

    /* 3) 提交失败也计入失败连击 (邮箱满/总线卡住) */
    if (s_can_tx.submit_fail != s_prev_submit_fail) {
        s_fail_streak += (s_can_tx.submit_fail - s_prev_submit_fail);
        s_prev_submit_fail = s_can_tx.submit_fail;
    }
    if (s_fail_streak >= APP_CAN_RECOVER_THRESHOLD) {
        g_recover_req = true;
        s_fail_streak = 0;
    }
    (void)now;
}

static void ctrl_apply_watchdogs(uint32_t now)
{
    /* ---- 桥接本地 FAILSAFE 帧: 立即撤销授权并失能 ---- */
    if (g_failsafe_events != s_seen_failsafe) {
        s_seen_failsafe = g_failsafe_events;
        lc_arm_force_safe(&s_arm);
        s_target_flags &= (uint16_t)~CTRL_FLAG_ENABLE;
        can_submit_disable();
        lc_can_tx_clear_motion(&s_can_tx);
        s_link_lost = true;
    }

    /* ---- CAN 恢复完成: 不允许自动恢复运动 ---- */
    if (g_can_recover_events != s_seen_recover) {
        s_seen_recover = g_can_recover_events;
        lc_arm_force_safe(&s_arm);
        lc_can_tx_abort(&s_can_tx);       /* 旧运动/参数目标作废 */
        s_target_flags &= (uint16_t)~CTRL_FLAG_ENABLE;
        s_fault = true;                    /* 需 CLEAR_ERR + 重新使能 */
        can_submit_disable();
    }

    /* ---- 控制链路看门狗: 300ms 无有效帧 -> 失能 (与文档统一, 无 1s 宽限) ---- */
    bool link_ok = g_have_ctrl &&
                   (lc_elapsed_ms(now, g_last_ctrl_ms) <= APP_CTRL_TIMEOUT_MS);
    if (!link_ok) {
        if (!s_link_lost) {
            s_link_lost = true;
            lc_arm_force_safe(&s_arm);
            s_target_flags &= (uint16_t)~CTRL_FLAG_ENABLE;
            can_submit_disable();
            lc_can_tx_clear_motion(&s_can_tx);
        }
    } else {
        s_link_lost = false;
    }

    /* ---- 电机反馈看门狗: 使能中 200ms 无反馈 -> 故障失能 ---- */
    fb_state_t fb;
    fb_read(&fb);
    uint32_t age = 0;
    bool fresh = fb_is_fresh(&fb, now, &age);
    bool grace = (int32_t)(now - s_fb_grace_until_ms) < 0;
    if (s_enabled && !fresh && !grace) {
        s_fault = true;
        lc_arm_force_safe(&s_arm);
        s_target_flags &= (uint16_t)~CTRL_FLAG_ENABLE;
        can_submit_disable();
        lc_can_tx_clear_motion(&s_can_tx);
    }

    /* ---- 电机自报故障 (err >= 0x8 才是故障; 0=失能 1=使能) ---- */
    if (fb.valid && fb.err >= 0x8u &&
        (int32_t)(now - s_fault_mask_until_ms) >= 0) {
        if (!s_fault) {
            s_fault = true;
            lc_arm_force_safe(&s_arm);
            s_target_flags &= (uint16_t)~CTRL_FLAG_ENABLE;
            can_submit_disable();
        }
    }
}

static void ctrl_step(uint32_t now)
{
    /* ---- 1. 命令: 安全命令优先, 其余按到达顺序 (带 100ms 有效期) ---- */
    for (;;) {
        lc_cmd_t cmd;
        bool got = false;
        if (xSemaphoreTake(s_cmdq_mtx, 0) == pdTRUE) {
            got = lc_cmdq_pop(&s_cmdq, now, APP_CMD_MAX_AGE_MS, NULL, &cmd);
            (void)xSemaphoreGive(s_cmdq_mtx);
        }
        if (!got) {
            break;
        }
        apply_command(&cmd, now);
    }

    /* ---- 2. 增益帧 ---- */
    gain_msg_t gm;
    while (xQueueReceive(s_gain_q, &gm, 0) == pdTRUE) {
        s_target_kp = clampf(gm.kp, MOTOR_KP_MIN, MOTOR_KP_MAX);
        s_target_kd = clampf(gm.kd, MOTOR_KD_MIN, MOTOR_KD_MAX);
        s_gains_rx++;
    }

    /* ---- 3. 参数请求 (低优先级 CAN) ---- */
    app_param_t pr;
    if (xQueueReceive(s_param_q, &pr, 0) == pdTRUE) {
        can_submit_param(&pr);
    }

    /* ---- 4. 看门狗/授权/故障 ---- */
    ctrl_apply_watchdogs(now);

    /* ---- 5. 期望使能状态 (所有条件同时满足才允许出力) ---- */
    fb_state_t fb;
    fb_read(&fb);
    uint32_t fb_age = 0;
    bool fb_fresh = fb_is_fresh(&fb, now, &fb_age);
    bool grace = (int32_t)(now - s_fb_grace_until_ms) < 0;
    bool fb_ok = fb_fresh || (!fb.valid && grace);

    bool want_enable = lc_arm_armed(&s_arm) && !s_estop && !s_fault &&
                       !s_link_lost && !can_drv_is_recovering() && fb_ok;

    /* ---- 6. 增益斜坡 ---- */
    if (want_enable) {
        if (s_ramp < GAIN_RAMP_STEPS) {
            s_ramp++;
        }
    } else if (s_ramp > 0u) {
        s_ramp = (s_ramp > GAIN_RAMP_DOWN) ? (uint16_t)(s_ramp - GAIN_RAMP_DOWN) : 0u;
    }
    float ratio = (float)s_ramp / (float)GAIN_RAMP_STEPS;
    s_kp_now = s_target_kp * ratio;
    s_kd_now = s_target_kd * ratio;

    /* ---- 7. 使能/失能边沿 ---- */
    if (want_enable && !s_enabled) {
        s_enabled = true;
        can_submit_enable();
        s_fail_streak = 0;
    } else if (!want_enable && s_enabled) {
        s_enabled = false;
        can_submit_disable();
        lc_can_tx_clear_motion(&s_can_tx);
    }

    /* ---- 8. 失能补发: 提交成功后 10ms 内再补 3 帧 (取代旧的"一次 50 帧") ---- */
    if (s_need_disable_burst) {
        uint32_t since = lc_elapsed_ms(now, s_disarm_ms);
        if (s_disable_sent >= (1u + APP_DISABLE_REPEAT) || since > 20u) {
            s_need_disable_burst = false;
        } else if (since >= 4u * s_disable_sent) {
            /* 每约 4ms 再排一帧; lc_can_tx 的同类安全槽会合并, 不会堆 50 帧 */
            uint8_t d[8];
            uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_DISABLE, d);
            can_request_safety(LC_CAN_SAFETY_DISABLE, id, d);
            s_disable_sent++;
        }
    }

    /* ---- 9. 组 MIT 帧 (每拍都发, 保持电机状态机连续) ---- */
    dm_mit_cmd_t mit;
    bool velocity_mode = (s_target_flags & CTRL_FLAG_VELOCITY) != 0u;
    float dt = s_motion_at ? (float)lc_elapsed_ms(now, s_motion_at) * 0.001f : 0.001f;
    s_motion_at = now;
    /* A zero-gain enable is a diagnostic enable, never a hidden trajectory. */
    bool drive = want_enable && (velocity_mode ? s_target_kd > 0.0f : s_target_kp > 0.0f);
    gm_step(&s_motion, drive, velocity_mode, s_target_pos_rad,
            s_target_vel_rad_s, fb.valid ? fb.pos_rad : 0.0f, dt,
            (velocity_mode ? GIMBAL_MANUAL_SPEED_MAX_DPS : GIMBAL_SPEED_MAX_DPS) * DEG2RAD, GIMBAL_ACCEL_DPS2 * DEG2RAD,
            GIMBAL_JERK_DPS3 * DEG2RAD, MOTOR_PMAX);
    mit.pos_rad   = velocity_mode ? 0.0f : s_motion.position;
    mit.vel_rad_s = s_motion.velocity * ratio;
    mit.kp        = velocity_mode ? 0.0f : s_kp_now;
    mit.kd        = s_kd_now;
    /* Match the DM MIT speed example: no direct torque feedforward. */
    mit.torque_nm = 0.0f;
    if (s_ramp == 0u) {
        mit.pos_rad = fb.valid ? fb.pos_rad : 0.0f;
    }

    lc_can_frame_t mf;
    memset(&mf, 0, sizeof(mf));
    mf.len  = 8u;
    mf.prio = (uint8_t)LC_CAN_PRIO_MOTION;
    mf.id   = dm_pack_mit(MOTOR_CAN_ID, &mit, mf.data);
    (void)lc_can_tx_request(&s_can_tx, &mf);

    /* ---- 10. CAN 发送协调 ---- */
    ctrl_service_can(now);

    /* ---- 11. 一致快照 ---- */
    app_rt_state_t snap;
    memset(&snap, 0, sizeof(snap));
    snap.target_deg    = s_target_pos_rad * RAD2DEG;
    snap.actual_deg    = fb.valid ? (fb.pos_rad * RAD2DEG) : 0.0f;
    snap.target_vel_dps = s_target_vel_rad_s * RAD2DEG;
    snap.kp            = mit.kp;
    snap.kd            = s_kd_now;
    snap.temp_mos      = fb.temp_mos;
    snap.err_code      = fb.err;
    snap.arm_token     = lc_arm_token(&s_arm);
    snap.flags         = s_target_flags;
    snap.ctrl_ticks    = g_ctrl_ticks;
    snap.cmd_drops     = (uint32_t)s_cmdq.push_drop_motion + s_cmdq.expired +
                         s_cmdq.push_drop_safety + g_link_drop_cmds;
    snap.fb_age_ms     = fb.valid ? fb_age : 0xFFFFFFFFu;
    snap.can_fail      = (uint16_t)((s_can_tx.submit_fail > 0xFFFFu)
                                    ? 0xFFFFu : s_can_tx.submit_fail);
    snap.armed         = s_arm.armed;
    snap.enabled       = s_enabled;
    snap.estop         = s_estop;
    snap.fault         = s_fault;
    snap.feedback_valid = fb.valid;
    snap.feedback_fresh = fb_fresh;
    snap.link_ok       = !s_link_lost;
    snap.can_recovering = can_drv_is_recovering();
    snap.arm_reject    = (s_arm.arm_reject > 0u);
    if (s_estop) {
        snap.safe_reason = APP_SAFE_ESTOP;
    } else if (s_fault) {
        snap.safe_reason = APP_SAFE_FAULT;
    } else if (s_link_lost) {
        snap.safe_reason = APP_SAFE_LINK_LOST;
    } else if (!fb_fresh && fb.valid) {
        snap.safe_reason = APP_SAFE_STM_FB;
    } else if (snap.can_recovering) {
        snap.safe_reason = APP_SAFE_CAN;
    } else {
        snap.safe_reason = APP_SAFE_NONE;
    }
    snap.gains_rx = s_gains_rx;

    taskENTER_CRITICAL();
    g_snap = snap;
    /* feedback 的"新鲜度"是时间函数, 显示层自己按年龄判断; 这里不改 fb */
    taskEXIT_CRITICAL();
}

static void ctrl_task(void *arg)
{
    (void)arg;

    /* 先把 1kHz 硬件节拍打开 (此时调度器已在运行, 中断里可以安全用 FromISR)。 */
    timer_drv_start_1khz(ctrl_tick_isr);

    for (;;) {
        uint32_t bits = 0;
        (void)xTaskNotifyWait(0, APP_NOTIFY_CTRL_TICK, &bits, portMAX_DELAY);
        ctrl_step(now_ms());
    }
}

/* ================================================================== */
/* 链路任务: DMA 字节 -> 成帧 -> 序号/过期 -> 命令队列                */
/* ================================================================== */

static void link_push_command(const ctrl_payload_t *pl, uint8_t seq, uint32_t rx_ms,
                              bool safety)
{
    lc_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.flags       = pl->flags;
    c.seq         = seq;
    c.mode        = pl->mode;
    c.yaw_q       = pl->yaw_q;
    c.pitch_q     = pl->pitch_q;
    c.yawrate_q   = pl->yawrate_q;
    c.pitchrate_q = pl->pitchrate_q;
    c.rx_ms       = rx_ms;
    c.safety      = safety ? 1u : 0u;

    if (xSemaphoreTake(s_cmdq_mtx, 0) != pdTRUE) {
        g_link_drop_cmds++;
        return;
    }
    bool ok = lc_cmdq_push(&s_cmdq, &c);
    (void)xSemaphoreGive(s_cmdq_mtx);
    if (!ok) {
        g_link_drop_cmds++;
    }
}

static void link_task(void *arg)
{
    (void)arg;

    lc_rx_t rx;
    lc_rx_init(&rx);
    lc_seq_gate_t gate;
    lc_seq_gate_init(&gate);

    uint8_t bytes[64];
    uint8_t frame[17];
    uint8_t last_enable = 0u;
    bool have_enable = false;

    for (;;) {
        vTaskDelay(1);   /* 1ms 轮询: 512B DMA 缓冲在 921600 下可撑 5.5ms, 余量足够 */

        if (link_uart_take_overflow()) {
            lc_rx_overflow(&rx);
        }

        uint16_t n;
        while ((n = link_uart_read(bytes, (uint16_t)sizeof(bytes))) > 0u) {
            uint32_t now = now_ms();
            for (uint16_t i = 0; i < n; i++) {
                lc_rx_kind_t kind = lc_rx_feed(&rx, bytes[i], now, frame);
                if (kind == LC_RX_NONE) {
                    continue;
                }

                switch (kind) {
                case LC_RX_CTRL: {
                    ctrl_payload_t pl;
                    uint8_t seq = 0;
                    if (!proto_decode_ctrl(frame, CTRL_FRAME_SIZE, &pl, &seq)) {
                        break;
                    }

                    if ((pl.flags & CTRL_FLAG_VELOCITY) && !(pl.flags & (CTRL_FLAG_FAILSAFE | CTRL_FLAG_ESTOP)) &&
                        (pl.yawrate_q < -(int)(MANUAL_YAW_MAX_DPS / PROTO_RATE_SCALE) ||
                         pl.yawrate_q > (int)(MANUAL_YAW_MAX_DPS / PROTO_RATE_SCALE))) {
                        g_link_drop_cmds++;
                        break;
                    }

                    /* 桥接本地注入的 FAILSAFE 帧: 不是主机原帧, 不参与序号窗口,
                     * 只作为"立即进入安全态"的事件。 */
                    if (pl.flags & CTRL_FLAG_FAILSAFE) {
                        g_failsafe_events++;
                        break;
                    }

                    /* 序号窗口: 重复/乱序/迟到/回绕都由 linkcore 统一处理 */
                    if (!lc_seq_accept(&gate, seq)) {
                        g_link_drop_cmds++;
                        break;
                    }

                    g_last_ctrl_ms = now;
                    g_have_ctrl = true;

                    bool enable = (pl.flags & CTRL_FLAG_ENABLE) != 0u;
                    bool safety = (pl.flags & (CTRL_FLAG_ESTOP | CTRL_FLAG_CLEAR_ERR |
                                               CTRL_FLAG_SET_ZERO)) != 0u ||
                                  !have_enable || (enable != (last_enable != 0u));
                    last_enable = enable ? 1u : 0u;
                    have_enable = true;

                    link_push_command(&pl, seq, now, safety);
                    break;
                }

                case LC_RX_GAINS: {
                    float kp = 0.0f, kd = 0.0f;
                    if (proto_decode_gains(frame, GAINS_FRAME_SIZE, &kp, &kd)) {
                        gain_msg_t gm = { kp, kd };
                        if (xQueueSend(s_gain_q, &gm, 0) != pdTRUE) {
                            g_link_drop_cmds++;
                        }
                    }
                    break;
                }

                case LC_RX_PARAM: {
                    uint8_t op, rid, val[4];
                    if (proto_decode_param(frame, CTRL_FRAME_SIZE, &op, &rid, val)) {
                        app_param_t pr;
                        pr.op = op;
                        pr.rid = rid;
                        memcpy(pr.value, val, 4);
                        if (xQueueSend(s_param_q, &pr, 0) != pdTRUE) {
                            g_link_drop_cmds++;
                        }
                    }
                    break;
                }

                default:
                    break;
                }
            }
        }
    }
}

/* ================================================================== */
/* CAN 任务: 接收 / 反馈 / 参数回包 / 总线恢复                        */
/* ================================================================== */

static void can_task(void *arg)
{
    (void)arg;
    uint32_t last_recover_ms = 0;
    uint32_t last_ticks = 0;
    uint32_t tick_stall = 0;

    for (;;) {
        vTaskDelay(1);

        /* 0. 控制节拍存活监控:
         *    TIM2 中断/ctrl 任务如果停摆, 电机就会一直按最后一条 CAN 目标出力。
         *    这里是独立于 ctrl 任务的最后一道兜底: 连续 50ms 没有控制节拍,
         *    立即尽力发失能帧并复位 (复位后默认未授权/失能)。 */
        uint32_t ticks_now = g_ctrl_ticks;
        if (ticks_now == last_ticks) {
            if (++tick_stall >= 50u) {
                app_emergency_disable_motor();
                NVIC_SystemReset();
            }
        } else {
            tick_stall = 0;
            last_ticks = ticks_now;
        }

        /* 1. 接收: 一次最多 8 帧, 防止洪泛占住任务 */
        can_frame_t f;
        uint8_t guard = 0;
        while (guard++ < 8u && can_drv_receive(&f)) {
            dm_feedback_t fb;
            dm_param_reply_t param;
            dm_rx_kind_t kind = dm_decode_can(MOTOR_CAN_ID, MOTOR_MASTER_ID,
                                             f.id, f.data, f.len, &fb, &param);
            if (kind == DM_RX_FEEDBACK) {
                fb_update(&fb);
            } else if (kind == DM_RX_PARAM_REPLY) {
                app_param_reply_t r;
                r.rid = param.rid;
                r.ok = 1u;
                memcpy(r.value, param.value, sizeof(r.value));
                if (xQueueSend(s_reply_q, &r, 0) != pdTRUE) {
                    g_link_drop_cmds++;
                }
            }
        }

        /* 2. 总线恢复: 由 ctrl 任务请求, 这里持锁执行; 恢复期间控制任务停发 */
        uint32_t now = now_ms();
        bool need = g_recover_req || can_drv_bus_off() || !can_drv_is_healthy();
        if (need && !can_drv_is_recovering() &&
            lc_elapsed_ms(now, last_recover_ms) >= APP_CAN_RECOVER_PERIOD_MS) {
            last_recover_ms = now;
            g_recover_req = false;
            can_drv_set_recovering(true);
            bool recovered = false;
            if (can_drv_lock(50)) {
                recovered = can_drv_recover();
                can_drv_abort_all();
                can_drv_unlock();
            }
            can_drv_set_recovering(false);
            g_can_recover_events++; /* Either outcome invalidates old authorization. */
            if (!recovered) g_recover_req = true;
            dbg_line(recovered ? "[CAN] recovered; explicit re-enable required"
                               : "[CAN] recovery failed; motion remains blocked");
        }
    }
}

/* ================================================================== */
/* 状态回传任务: 唯一 UART 发送者                                     */
/* ================================================================== */

static uint8_t motor_safety_bits(const app_rt_state_t *st)
{
    uint8_t s = 0;
    if (st->armed)           s |= PROTO_MOTOR_SAFETY_ARMED;
    /* Enabled is motor feedback, not merely the requested software state. */
    if (st->feedback_fresh && st->err_code == 1u) s |= PROTO_MOTOR_SAFETY_ENABLED;
    if (st->estop)           s |= PROTO_MOTOR_SAFETY_ESTOP;
    if (st->fault)           s |= PROTO_MOTOR_SAFETY_FAULT;
    if (!st->feedback_fresh) s |= PROTO_MOTOR_SAFETY_FBK_STALE;
    if (st->can_recovering)  s |= PROTO_MOTOR_SAFETY_CAN_RECOVER;
    if (!st->link_ok)        s |= PROTO_MOTOR_SAFETY_LINK_LOST;
    if (st->arm_reject)      s |= PROTO_MOTOR_SAFETY_ARM_REJECT;
    return s;
}

static void status_task(void *arg)
{
    (void)arg;
    TickType_t last_wake = xTaskGetTickCount();
    uint32_t last_motor_ms = 0;

    for (;;) {
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(APP_STATUS_PERIOD_MS));
        uint32_t now = now_ms();

        app_rt_state_t st;
        snap_load(&st);
        fb_state_t fb;
        fb_read(&fb);

        /* 1. 50Hz 状态帧 */
        status_frame_t sf;
        memset(&sf, 0, sizeof(sf));
        if (st.estop) {
            sf.state = LINK_STATE_ESTOP;
        } else if (st.fault) {
            sf.state = LINK_STATE_FAULT;
        } else if (!st.link_ok) {
            sf.state = LINK_STATE_TIMEOUT;
        } else if (st.armed && st.enabled) {
            sf.state = LINK_STATE_OK;
        } else {
            sf.state = LINK_STATE_INIT;   /* 待机(未使能), 不再误报"失联" */
        }
        sf.rssi      = 0;   /* 由桥接端填 (2.x 框架回调拿不到 RSSI, 见文档) */
        sf.yaw_deg   = fb.valid ? (fb.pos_rad * RAD2DEG) : 0.0f;
        sf.pitch_deg = 0.0f;
        sf.temp_mos  = fb.temp_mos;
        sf.err_code  = fb.err;
        link_uart_send_status(&sf, s_tx_seq++);

        /* 2. 20Hz 电机实时量 (带安全位/令牌回显, 让主机确认"谁被授权") */
        if (lc_elapsed_ms(now, last_motor_ms) >= APP_MOTOR_PERIOD_MS) {
            last_motor_ms = now;
            link_uart_send_motor(fb.valid ? fb.pos_rad : 0.0f,
                                 fb.valid ? fb.vel_rad_s : 0.0f,
                                 fb.valid ? fb.torque_nm : 0.0f,
                                 st.can_fail,
                                 motor_safety_bits(&st),
                                 (uint8_t)(st.cmd_drops & 0xFFu),
                                 st.arm_token,
                                 s_tx_seq++);
        }

        /* 3. 参数回包 */
        app_param_reply_t r;
        while (xQueueReceive(s_reply_q, &r, 0) == pdTRUE) {
            link_uart_send_param_reply(r.rid, r.ok != 0u, r.value, s_tx_seq++);
        }
    }
}

/* ================================================================== */
/* UI 任务: OLED + 调试日志 + 栈余量诊断                              */
/* ================================================================== */

static void ui_task(void *arg)
{
    (void)arg;

    bool disp_ok = display_init();
    uint32_t last_log = 0;
    uint32_t last_diag = 0;

    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_UI_PERIOD_MS));
        uint32_t now = now_ms();

        if (disp_ok) {
            display_tick(now);
        }

        if (lc_elapsed_ms(now, last_log) >= APP_LOG_PERIOD_MS) {
            last_log = now;
            app_rt_state_t st;
            snap_load(&st);
            const char *s = "OFF";
            if (st.estop)       s = "EST";
            else if (st.fault)  s = "FLT";
            else if (st.armed && st.enabled) s = "ON";
            dbg_printf("[%lu] %s arm=%u en=%u link=%u canrec=%u drop=%lu ovf=%lu ticks=%lu",
                       (unsigned long)now, s,
                       st.armed ? 1u : 0u, st.enabled ? 1u : 0u,
                       st.link_ok ? 1u : 0u, st.can_recovering ? 1u : 0u,
                       (unsigned long)st.cmd_drops,
                       (unsigned long)link_uart_overflow_count(),
                       (unsigned long)st.ctrl_ticks);
        }

        /* 栈余量诊断: 现场判断"任务栈够不够"的唯一可靠依据 */
        if (lc_elapsed_ms(now, last_diag) >= APP_STACK_DIAG_PERIOD_MS) {
            last_diag = now;
            dbg_printf("[STACK] free_words ctrl=%u link=%u can=%u status=%u ui=%u",
                       (unsigned)uxTaskGetStackHighWaterMark(s_ctrl_task),
                       (unsigned)uxTaskGetStackHighWaterMark(s_link_task_h),
                       (unsigned)uxTaskGetStackHighWaterMark(s_can_task_h),
                       (unsigned)uxTaskGetStackHighWaterMark(s_status_task_h),
                       (unsigned)uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

/* ================================================================== */
/* 对外接口                                                           */
/* ================================================================== */

void app_emergency_disable_motor(void)
{
    /* 致命错误路径: 不取锁、不等发送完成, 尽力把失能命令塞进邮箱。
     * 随后调用方 (rtos_start.c 的钩子) 会立刻复位, 复位后默认未授权/失能。 */
    for (uint8_t i = 0; i < 3u; i++) {
        uint8_t d[8];
        uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_DISABLE, d);
        (void)can_drv_send(id, d, 8u);
    }
}

void app_setup(void)
{
    /* ---- 1. 外设 ---- */
    link_uart_init();
    bool can_ok = can_drv_init();
    dbg_uart_init();
    dbg_line("[BOOT] STM32F103 靶车固件启动 (FreeRTOS, USART2 调试口)");
    if (!can_ok) {
        dbg_line("[CAN] 初始化失败: 控制任务会停发, 等待恢复");
    }

    /* ---- 2. 队列/互斥量 (全部静态分配, 创建失败 = 配置错误) ---- */
    s_cmdq_mtx = xSemaphoreCreateMutexStatic(&s_cmdq_mtx_buf);
    s_gain_q   = xQueueCreateStatic(2u, sizeof(gain_msg_t), s_gain_qstore, &s_gain_qbuf);
    s_param_q  = xQueueCreateStatic(2u, sizeof(app_param_t), s_param_qstore, &s_param_qbuf);
    s_reply_q  = xQueueCreateStatic(2u, sizeof(app_param_reply_t), s_reply_qstore, &s_reply_qbuf);

    if (s_cmdq_mtx == NULL || s_gain_q == NULL || s_param_q == NULL || s_reply_q == NULL) {
        dbg_line("[FATAL] 队列/互斥量创建失败");
        app_emergency_disable_motor();
        for (;;) {
            /* 配置错误: 不启动调度器, 保持电机失能 */
        }
    }

    lc_cmdq_init(&s_cmdq);
    lc_arm_init(&s_arm);
    lc_can_tx_init(&s_can_tx);
    memset(&g_fb, 0, sizeof(g_fb));
    memset(&g_snap, 0, sizeof(g_snap));
    s_fb_grace_until_ms = APP_FB_GRACE_MS;   /* 上电后 1s 内允许无反馈 */

    /* ---- 3. 任务 (静态分配, 优先级/栈见 app_tasks.h) ---- */
    s_ctrl_task = xTaskCreateStatic(ctrl_task, "ctrl", APP_STACK_CTRL_WORDS, NULL,
                                    APP_PRIO_CTRL, s_ctrl_stack, &s_ctrl_tcb);
    if (s_ctrl_task == NULL) {
        dbg_line("[FATAL] ctrl 任务创建失败");
        app_emergency_disable_motor();
        for (;;) { }
    }
    s_link_task_h   = xTaskCreateStatic(link_task, "link", APP_STACK_LINK_WORDS, NULL,
                                        APP_PRIO_LINK, s_link_stack, &s_link_tcb);
    s_can_task_h    = xTaskCreateStatic(can_task, "can", APP_STACK_CAN_WORDS, NULL,
                                        APP_PRIO_CAN, s_can_stack, &s_can_tcb);
    s_status_task_h = xTaskCreateStatic(status_task, "status", APP_STACK_STATUS_WORDS, NULL,
                                        APP_PRIO_STATUS, s_status_stack, &s_status_tcb);
    s_ui_task_h = xTaskCreateStatic(ui_task, "ui", APP_STACK_UI_WORDS, NULL,
                                    APP_PRIO_UI, s_ui_stack, &s_ui_tcb);
    if (s_link_task_h == NULL || s_can_task_h == NULL ||
        s_status_task_h == NULL || s_ui_task_h == NULL) {
        dbg_line("[FATAL] 任务创建失败");
        app_emergency_disable_motor();
        for (;;) { }
    }

    /* ---- 4. 启动调度器 (正常情况下不返回) ---- */
    vTaskStartScheduler();

    /* 走到这里说明调度器启动失败 (通常是静态内存不足) */
    dbg_line("[FATAL] FreeRTOS 调度器启动失败");
    app_emergency_disable_motor();
    for (;;) { }
}

void app_get_status(app_status_t *out)
{
    if (out == NULL) {
        return;
    }
    app_rt_state_t st;
    fb_state_t fb;
    snap_load(&st);
    fb_read(&fb);

    out->target_deg     = st.target_deg;
    out->actual_deg     = st.actual_deg;
    out->target_vel_dps = st.target_vel_dps;
    out->kp             = st.kp;
    out->kd             = st.kd;
    out->temp_mos       = st.temp_mos;
    out->err_code       = st.err_code;
    out->gains_rx       = st.gains_rx;
    out->enabled        = st.enabled;
    out->fault          = st.fault;
    out->estop          = st.estop;
    out->feedback_valid = st.feedback_valid;
    out->ctrl_ticks     = st.ctrl_ticks;
    (void)fb;
}
