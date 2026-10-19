/**
 * @file    app_main.c
 * @brief   靶车 —— STM32F103C8T6 控制器主逻辑
 *
 *  运行时结构:
 *
 *    USART1_IRQHandler  ──► 解析 17 字节控制帧, 校验通过才置 pending
 *                                    │
 *    TIM2_IRQHandler (1kHz) ────────┼──► 控制节拍:
 *                                    │      看门狗 -> 增益斜坡 -> MIT 打包 -> CAN 发
 *    main loop ─────────────────────┘
 *         ├── 取走 pending 控制帧, 更新目标与截止时间
 *         ├── 50Hz 发反馈帧给桥接
 *         └── 处理 CAN 反馈 (位置/温度/错误码)
 *
 *  ★ 三层安全, 任何一层单独失效都不会让云台失控:
 *      1. 桥接端: 主机失联 300ms -> 每 50ms 注入失能帧
 *      2. 本文件: 自己也有 300ms 看门狗 (不依赖桥接), 超时立即 kp/kd 归零并失能
 *      3. 增益斜坡: 使能/失能都走 500ms 斜坡, 避免阶跃把电机拽响
 */

#include "app_main.h"
#include "config.h"
#include "protocol.h"
#include "link_uart.h"
#include "can_drv.h"
#include "dm_motor.h"
#include "timer_drv.h"
#include "display.h"

/* HAL 只需用到 HAL_GetTick() 做毫秒时基 (由 Arduino 核心的 SysTick 驱动),
 * 外设全部走裸寄存器, 所以只包这一个头。 */
#include "stm32f1xx_hal.h"

#include <math.h>
#include <string.h>

/* ================================================================== */
/* 常量                                                               */
/* ================================================================== */

#define CTRL_TICK_HZ        1000u
#define GAIN_RAMP_STEPS     500u      /* 增益斜坡步数: 500 步 @1kHz = 500ms */
#define CAN_FAIL_LIMIT      200u      /* 连续发不出去多少次判定总线故障 */
#define DISABLE_BURST       50u       /* 失能时连发多少帧失能命令 */

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif
#define DEG2RAD  ((float)M_PI / 180.0f)
#define RAD2DEG  (180.0f / (float)M_PI)

/* ================================================================== */
/* 状态 (ISR 与主循环共享, 都是 32 位对齐标量, 读写天然原子)          */
/* ================================================================== */

/* ---- 目标 (主循环写, ISR 读) ---- */
static volatile float    s_target_pos_rad   = 0.0f;
static volatile float    s_target_vel_rad_s = 0.0f;
static volatile float    s_target_kp        = MOTOR_KP_DEFAULT;
static volatile float    s_target_kd        = MOTOR_KD_DEFAULT;
static volatile uint16_t s_target_flags     = 0;
static volatile uint32_t s_link_deadline_ms = 0;
static volatile bool     s_estop            = false;

/* ---- 增益斜坡 (只用 ISR) ---- */
static float    s_kp_now = 0.0f;
static float    s_kd_now = 0.0f;
static uint16_t s_ramp   = 0;         /* 0..GAIN_RAMP_STEPS */
static bool     s_enabled = false;

/* ---- 反馈 (ISR 写, 主循环读) ---- */
static volatile float   s_fb_pos_rad  = 0.0f;
static volatile float   s_fb_vel_rps  = 0.0f;
static volatile float   s_fb_torque   = 0.0f;
static volatile uint8_t s_fb_temp_mos = 0;
static volatile uint8_t s_fb_err      = 0;
static volatile bool    s_fb_valid    = false;
static volatile bool    s_fault       = false;
static volatile uint32_t s_can_fail   = 0;
static volatile uint32_t s_ctrl_ticks = 0;

/* ---- 主循环用 ---- */
static uint32_t s_last_status_ms = 0;
static bool     s_first_enable   = true;

/* ================================================================== */
/* 工具                                                               */
/* ================================================================== */

static inline float clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t now_ms(void)
{
    return HAL_GetTick();
}

/* ================================================================== */
/* 控制节拍 (TIM2 中断, 1kHz)                                         */
/* ================================================================== */

static void control_tick(void)
{
    s_ctrl_ticks++;

    /* ---- 1. 看门狗: 链路超时立刻断使能 (不依赖桥接端的注入帧) ---- */
    bool link_ok = (int32_t)(now_ms() - s_link_deadline_ms) < 0;

    /* 用 ticks 判断会更稳 (与中断同源), 但主循环喂的是毫秒, 这里统一用毫秒 */
    if (!link_ok) {
        s_target_flags &= (uint16_t)~CTRL_FLAG_ENABLE;
    }

    /* ---- 2. 一次性的清错/保存零点 ---- */
    if (s_target_flags & CTRL_FLAG_CLEAR_ERR) {
        uint8_t d[8];
        uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_CLEAR_ERR, d);
        can_drv_send(id, d, 8);
        s_fault = false;
        s_can_fail = 0;
        s_target_flags &= (uint16_t)~CTRL_FLAG_CLEAR_ERR;
    }
    if (s_target_flags & CTRL_FLAG_SET_ZERO) {
        uint8_t d[8];
        uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_SAVE_ZERO, d);
        can_drv_send(id, d, 8);
        s_target_flags &= (uint16_t)~CTRL_FLAG_SET_ZERO;
    }

    /* ---- 3. 急停优先级最高 ---- */
    bool want_enable = false;
    if (s_estop) {
        want_enable = false;
    } else if (link_ok && !s_fault) {
        want_enable = (s_target_flags & CTRL_FLAG_ENABLE) != 0;
    }

    /* ---- 4. 增益斜坡 ---- */
    if (want_enable) {
        if (s_ramp < GAIN_RAMP_STEPS) {
            s_ramp++;
        }
    } else {
        if (s_ramp > 0) {
            /* 断使能时斜坡要快: 200 步 = 200ms 内把增益收干净 */
            s_ramp = (s_ramp > 4u) ? (uint16_t)(s_ramp - 5u) : 0u;
        }
    }

    float ratio = (float)s_ramp / (float)GAIN_RAMP_STEPS;
    s_kp_now = s_target_kp * ratio;
    s_kd_now = s_target_kd * ratio;

    /* ---- 5. 组帧 ---- */
    dm_mit_cmd_t cmd;
    cmd.pos_rad   = s_target_pos_rad;
    cmd.vel_rad_s = s_target_vel_rad_s * ratio;   /* 速度前馈也一起斜坡 */
    cmd.kp        = s_kp_now;
    cmd.kd        = s_kd_now;
    cmd.torque_nm = MOTOR_TORQUE_FF * ratio;

    /* 完全没使能时: 位置给当前反馈位置, 增益 0 -> 电机自由, 不会突然出力 */
    if (s_ramp == 0u) {
        cmd.pos_rad = s_fb_valid ? s_fb_pos_rad : 0.0f;
    }

    uint8_t data[8];
    uint32_t can_id = dm_pack_mit(MOTOR_CAN_ID, &cmd, data);

    bool sent = can_drv_send(can_id, data, 8);
    if (!sent) {
        if (s_can_fail < 0xFFFFu) {
            s_can_fail++;
        }
        if (s_can_fail > CAN_FAIL_LIMIT) {
            s_fault = true;
        }
    } else {
        if (s_can_fail > 0u) {
            s_can_fail--;
        }
    }

    /* ---- 6. 使能/失能状态跳变时补发命令 ---- */
    if (want_enable && !s_enabled) {
        s_enabled = true;
        s_first_enable = false;
        uint8_t d[8];
        uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_ENABLE, d);
        can_drv_send(id, d, 8);
    } else if (!want_enable && s_enabled) {
        /* 断使能: 连发多帧, 丢一两帧也能确保电机松掉 */
        for (uint8_t i = 0; i < DISABLE_BURST; i++) {
            uint8_t d[8];
            uint32_t id = dm_pack_special(MOTOR_CAN_ID, DM_MODE_MIT, DM_CMD_DISABLE, d);
            if (!can_drv_send(id, d, 8)) {
                break;
            }
        }
        s_enabled = false;
    }
}

/* 中断服务函数不能自己抢:
 * HardwareTimer.cpp 里已经定义了 TIM2_IRQHandler, 再定义一个会链接失败。
 * 定时器初始化交给 timer_drv (C++ 包装), 主逻辑保持纯 C。 */

static void timer_init_1khz(void)
{
    timer_drv_start_1khz(control_tick);
}

/* ================================================================== */
/* 命令处理 (主循环)                                                  */
/* ================================================================== */

static void apply_command(const link_cmd_t *cmd)
{
    ctrl_target_t t;
    proto_payload_to_target(&cmd->payload, &t);

    /* 机械限位夹紧 —— 协议层的量程是 ±180/±90, 这里要收到云台真实行程 */
    float yaw_deg   = clampf(t.yaw_deg,   GIMBAL_YAW_MIN_DEG,   GIMBAL_YAW_MAX_DEG);
    float pitch_deg = clampf(t.pitch_deg, GIMBAL_PITCH_MIN_DEG, GIMBAL_PITCH_MAX_DEG);

    /* 度 -> 弧度, 再夹到电机量程 */
    float pos_rad = clampf(yaw_deg * DEG2RAD, -MOTOR_PMAX, MOTOR_PMAX);
    float vel_rps = clampf(t.yaw_rate_dps * DEG2RAD, -MOTOR_VMAX, MOTOR_VMAX);

    s_target_pos_rad   = pos_rad;
    s_target_vel_rad_s = vel_rps;
    s_target_flags     = cmd->payload.flags;

    /* 急停由主机标志位驱动 */
    s_estop = (cmd->payload.flags & CTRL_FLAG_ESTOP) != 0;

    /* 只有收到合法帧才续命; 桥接的失能帧没有 ENABLE 位, 也会走到这里,
     * 但那时 deadline 同样被刷新是错的 —— 所以失能帧不续命: */
    if (cmd->payload.flags & CTRL_FLAG_ENABLE) {
        s_link_deadline_ms = now_ms() + CTRL_TIMEOUT_MS;
    }

    /* 俯仰轴: 当前只接了单轴云台。如果以后加第二台电机,
     * 在这里把 pitch_deg 也转成弧度并驱动第二路 CAN ID 即可。 */
    (void)pitch_deg;
}

/* ================================================================== */
/* 反馈帧 (50Hz)                                                      */
/* ================================================================== */

static void send_status(void)
{
    status_frame_t st;
    memset(&st, 0, sizeof(st));

    st.rssi = 0;   /* RSSI 由桥接端填, STM32 不知道无线链路质量 */

    if (s_estop) {
        st.state = LINK_STATE_ESTOP;
    } else if (s_fault) {
        st.state = LINK_STATE_FAULT;
    } else if ((s_target_flags & CTRL_FLAG_ENABLE) && s_enabled) {
        st.state = LINK_STATE_OK;
    } else {
        st.state = LINK_STATE_TIMEOUT;
    }

    st.yaw_deg   = s_fb_valid ? (s_fb_pos_rad * RAD2DEG) : 0.0f;
    st.pitch_deg = 0.0f;
    st.temp_mos  = s_fb_temp_mos;
    st.err_code  = s_fb_err;

    link_uart_send_status(&st);
}

/* ================================================================== */
/* CAN 反馈处理                                                       */
/* ================================================================== */

static void poll_can(void)
{
    can_frame_t f;
    uint8_t guard = 0;

    /* 一次最多处理 8 帧, 避免主循环被 CAN 洪泛卡住 */
    while (guard++ < 8u && can_drv_receive(&f)) {
        dm_feedback_t fb;
        if (dm_parse_feedback(MOTOR_CAN_ID, f.data, f.len, &fb)) {
            s_fb_pos_rad  = fb.pos_rad;
            s_fb_vel_rps  = fb.vel_rad_s;
            s_fb_torque   = fb.torque_nm;
            s_fb_temp_mos = fb.temp_mos;
            s_fb_err      = fb.err;
            s_fb_valid    = true;

            /* 电机自报错误 -> 置故障, 需要主机发 CLEAR_ERR 才能恢复 */
            if (fb.err != 0u) {
                s_fault = true;
            }
        }
    }
}

/* ================================================================== */
/* 只读状态快照 (给 OLED 显示用)                                      */
/* ================================================================== */

void app_get_status(app_status_t *out)
{
    if (out == NULL) {
        return;
    }
    out->target_deg     = s_target_pos_rad * RAD2DEG;
    out->actual_deg     = s_fb_valid ? (s_fb_pos_rad * RAD2DEG) : 0.0f;
    out->target_vel_dps = s_target_vel_rad_s * RAD2DEG;
    out->kp             = s_kp_now;
    out->kd             = s_kd_now;
    out->temp_mos       = s_fb_temp_mos;
    out->err_code       = s_fb_err;
    out->enabled        = s_enabled;
    out->fault          = s_fault;
    out->estop          = s_estop;
    out->feedback_valid = s_fb_valid;
    out->ctrl_ticks     = s_ctrl_ticks;
}

/* ================================================================== */
/* 对外接口                                                           */
/* ================================================================== */

void app_setup(void)
{
    link_uart_init();
    can_drv_init();

    s_link_deadline_ms = now_ms() + CTRL_TIMEOUT_MS;
    s_target_pos_rad   = 0.0f;
    s_target_kp        = MOTOR_KP_DEFAULT;
    s_target_kd        = MOTOR_KD_DEFAULT;

    timer_init_1khz();   /* 最后开控制节拍, 保证前面都初始化好了 */
}

void app_loop(void)
{
    uint32_t now = now_ms();

    /* 给接收打时间戳 */
    link_uart_tick_update(now);

    /* 把串口缓冲里的字节喂给协议状态机 */
    link_uart_poll();

    /* ---- 1. 取控制命令 ---- */
    link_cmd_t cmd;
    while (link_uart_take_command(&cmd)) {
        apply_command(&cmd);
    }

    /* ---- 2. CAN 反馈 ---- */
    poll_can();

    /* ---- 3. 50Hz 反馈帧 ---- */
    if ((uint32_t)(now - s_last_status_ms) >= STATUS_PERIOD_MS) {
        s_last_status_ms = now;
        send_status();
    }

    /* ---- 4. OLED 状态显示 (内部自带限速, 没接屏幕时自动跳过) ---- */
    display_tick(now);

    /* ★ 想看 STM32 的实时状态, 请看桥接端 (M5Stamp) 的串口监视器:
     *   它的日志里已经带了 STM32 回传的角度/温度/错误码。
     *   原因: PA9/PA10 被 USART1 链路占用, 本芯片没有第二路可用的调试串口。 */
}
