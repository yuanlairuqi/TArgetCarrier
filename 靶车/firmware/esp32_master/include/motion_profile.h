/**
 * @file    motion_profile.h
 * @brief   云台自主运动轨迹发生器 (纯 C, 可单元测试)
 *
 *  所有轨迹输出 角度 + 角速度(前馈), 角速度用于 MIT 模式的速度前馈,
 *  能显著减小跟踪滞后。
 */

#ifndef MOTION_PROFILE_H
#define MOTION_PROFILE_H

#include <stdint.h>
#include <stdbool.h>
#include "protocol.h"
#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 手动模式下目标角度的最大变化率, 防止串口下发的阶跃把电机拽响 */
#define SERIAL_MANUAL_MAX_DPS_LOCAL   180.0f

typedef struct {
    profile_id_t id;

    /* 通用 */
    float   t_s;              /* 轨迹内部时间 (秒) */
    float   center_yaw_deg;
    float   center_pitch_deg;

    /* SINE */
    float   sine_amp_deg;
    float   sine_period_s;

    /* SWEEP */
    float   sweep_amp_deg;
    float   sweep_speed_dps;
    uint32_t sweep_dwell_ms;
    float   sweep_pos_deg;
    int8_t  sweep_dir;
    uint32_t sweep_dwell_until_ms;

    /* CIRCLE */
    float   circle_radius_deg;
    float   circle_period_s;

    /* FIGURE8 */
    float   fig8_yaw_amp_deg;
    float   fig8_pitch_amp_deg;
    float   fig8_period_s;

    /* MANUAL / STATIC */
    float   manual_yaw_deg;
    float   manual_pitch_deg;
    float   last_yaw_deg;      /* 上一帧输出, 手动模式限速用 */
    float   last_pitch_deg;
} motion_profile_t;

void motion_profile_init(motion_profile_t *mp);

/**
 * @brief 切换轨迹; 切换时把当前角度作为新轨迹的中心, 避免突变
 */
void motion_profile_set(motion_profile_t *mp, profile_id_t id,
                        float current_yaw_deg, float current_pitch_deg);

/**
 * @brief 推进轨迹并输出目标
 * @param dt_s 距上次调用的时间 (秒)
 */
void motion_profile_step(motion_profile_t *mp, float dt_s, ctrl_target_t *out);

/** 手动模式: 直接指定角度 */
void motion_profile_set_manual(motion_profile_t *mp, float yaw_deg, float pitch_deg);

const char *motion_profile_name(profile_id_t id);

#ifdef __cplusplus
}
#endif

#endif /* MOTION_PROFILE_H */
