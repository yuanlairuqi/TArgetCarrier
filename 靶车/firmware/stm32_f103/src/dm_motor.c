/**
 * @file    dm_motor.c
 * @brief   达妙 DM 电机 MIT 协议实现
 *
 *  字节布局 (与官方 dm_motor_drv.c 的 mit_ctrl 完全一致):
 *    data[0] = pos_tmp >> 8
 *    data[1] = pos_tmp & 0xFF
 *    data[2] = vel_tmp >> 4
 *    data[3] = ((vel_tmp & 0xF) << 4) | (kp_tmp >> 8)
 *    data[4] = kp_tmp & 0xFF
 *    data[5] = kd_tmp >> 4
 *    data[6] = ((kd_tmp & 0xF) << 4) | (tor_tmp >> 8)
 *    data[7] = tor_tmp & 0xFF
 */

#include "dm_motor.h"
#include "config.h"
#include <math.h>

/* 达妙固件的固定增益量程 */
#define DM_KP_MIN   0.0f
#define DM_KP_MAX   500.0f
#define DM_KD_MIN   0.0f
#define DM_KD_MAX   5.0f

static inline float clampf_local(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

uint16_t dm_float_to_uint(float x, float x_min, float x_max, uint8_t bits)
{
    float span   = x_max - x_min;
    float offset = x_min;
    float v      = clampf_local(x, x_min, x_max);
    /* 不用位运算 1<<bits 的 int 版本, 避免 bits=16 时的隐式提升歧义 */
    float max_i  = (float)((1u << bits) - 1u);
    return (uint16_t)((v - offset) * max_i / span);
}

float dm_uint_to_float(uint16_t x, float x_min, float x_max, uint8_t bits)
{
    float span  = x_max - x_min;
    float max_i = (float)((1u << bits) - 1u);
    return ((float)x) * span / max_i + x_min;
}

uint32_t dm_pack_mit(uint8_t id, const dm_mit_cmd_t *cmd, uint8_t *out)
{
    uint16_t pos_tmp = dm_float_to_uint(cmd->pos_rad,   -MOTOR_PMAX, MOTOR_PMAX, 16);
    uint16_t vel_tmp = dm_float_to_uint(cmd->vel_rad_s, -MOTOR_VMAX, MOTOR_VMAX, 12);
    uint16_t tor_tmp = dm_float_to_uint(cmd->torque_nm, -MOTOR_TMAX, MOTOR_TMAX, 12);
    uint16_t kp_tmp  = dm_float_to_uint(cmd->kp, DM_KP_MIN, DM_KP_MAX, 12);
    uint16_t kd_tmp  = dm_float_to_uint(cmd->kd, DM_KD_MIN, DM_KD_MAX, 12);

    out[0] = (uint8_t)(pos_tmp >> 8);
    out[1] = (uint8_t)(pos_tmp & 0xFFu);
    out[2] = (uint8_t)(vel_tmp >> 4);
    out[3] = (uint8_t)(((vel_tmp & 0xFu) << 4) | (kp_tmp >> 8));
    out[4] = (uint8_t)(kp_tmp & 0xFFu);
    out[5] = (uint8_t)(kd_tmp >> 4);
    out[6] = (uint8_t)(((kd_tmp & 0xFu) << 4) | (tor_tmp >> 8));
    out[7] = (uint8_t)(tor_tmp & 0xFFu);

    return (uint32_t)DM_MODE_MIT + id;
}

uint32_t dm_pack_special(uint8_t id, dm_mode_t mode, uint8_t cmd_byte, uint8_t *out)
{
    for (uint8_t i = 0; i < 7u; i++) {
        out[i] = 0xFFu;
    }
    out[7] = cmd_byte;
    return (uint32_t)mode + id;
}

bool dm_parse_feedback(uint8_t motor_id, const uint8_t *data, uint8_t len,
                       dm_feedback_t *fb)
{
    if (len < 8u) {
        return false;
    }

    uint8_t id    = (uint8_t)(data[0] & 0x0Fu);
    uint8_t err   = (uint8_t)(data[0] >> 4);

    if (id != (motor_id & 0x0Fu)) {
        return false;   /* 不是我们这台电机的反馈 */
    }

    uint16_t p_int = (uint16_t)(((uint16_t)data[1] << 8) | data[2]);
    uint16_t v_int = (uint16_t)(((uint16_t)data[3] << 4) | (data[4] >> 4));
    uint16_t t_int = (uint16_t)((((uint16_t)data[4] & 0x0Fu) << 8) | data[5]);

    fb->id        = id;
    fb->err       = err;
    fb->pos_rad   = dm_uint_to_float(p_int, -MOTOR_PMAX, MOTOR_PMAX, 16);
    fb->vel_rad_s = dm_uint_to_float(v_int, -MOTOR_VMAX, MOTOR_VMAX, 12);
    fb->torque_nm = dm_uint_to_float(t_int, -MOTOR_TMAX, MOTOR_TMAX, 12);
    fb->temp_mos  = data[6];
    fb->temp_coil = data[7];

    return true;
}
