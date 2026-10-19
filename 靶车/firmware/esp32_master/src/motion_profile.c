/**
 * @file    motion_profile.c
 * @brief   云台自主运动轨迹发生器实现
 */

#include "motion_profile.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846f
#endif

#define TWO_PI (2.0f * (float)M_PI)

/* 角度限幅工具 */
static inline float clamp_angle(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline void clamp_target(ctrl_target_t *t)
{
    t->yaw_deg   = clamp_angle(t->yaw_deg,   GIMBAL_YAW_MIN_DEG,   GIMBAL_YAW_MAX_DEG);
    t->pitch_deg = clamp_angle(t->pitch_deg, GIMBAL_PITCH_MIN_DEG, GIMBAL_PITCH_MAX_DEG);
}

void motion_profile_init(motion_profile_t *mp)
{
    memset(mp, 0, sizeof(*mp));

    mp->id                = PROFILE_DEFAULT;
    mp->t_s               = 0.0f;

    mp->center_yaw_deg    = SINE_CENTER_DEG;
    mp->center_pitch_deg  = 0.0f;

    mp->sine_amp_deg      = SINE_AMPLITUDE_DEG;
    mp->sine_period_s     = SINE_PERIOD_S;

    mp->sweep_amp_deg     = SWEEP_AMPLITUDE_DEG;
    mp->sweep_speed_dps   = SWEEP_SPEED_DPS;
    mp->sweep_dwell_ms    = SWEEP_DWELL_MS;
    mp->sweep_pos_deg     = -SWEEP_AMPLITUDE_DEG;
    mp->sweep_dir         = +1;
    mp->sweep_dwell_until_ms = 0;

    mp->circle_radius_deg = CIRCLE_RADIUS_DEG;
    mp->circle_period_s   = CIRCLE_PERIOD_S;

    mp->fig8_yaw_amp_deg  = FIG8_YAW_AMP_DEG;
    mp->fig8_pitch_amp_deg= FIG8_PITCH_AMP_DEG;
    mp->fig8_period_s     = FIG8_PERIOD_S;

    mp->manual_yaw_deg    = 0.0f;
    mp->manual_pitch_deg  = 0.0f;
}

void motion_profile_set(motion_profile_t *mp, profile_id_t id,
                        float current_yaw_deg, float current_pitch_deg)
{
    if (id >= PROFILE_COUNT) {
        return;
    }

    /* 把当前实际角度接管成新轨迹的中心/起点, 保证切换瞬间目标连续 */
    mp->center_yaw_deg   = current_yaw_deg;
    mp->center_pitch_deg = current_pitch_deg;

    switch (id) {
    case PROFILE_STATIC:
    case PROFILE_MANUAL:
        mp->manual_yaw_deg   = current_yaw_deg;
        mp->manual_pitch_deg = current_pitch_deg;
        break;

    case PROFILE_SWEEP:
        /* 从当前位置出发, 朝较近的一端更自然: 这里统一从负端开始 */
        mp->sweep_pos_deg = clamp_angle(current_yaw_deg,
                                        -mp->sweep_amp_deg, mp->sweep_amp_deg);
        mp->sweep_dir = (mp->sweep_pos_deg > 0.0f) ? -1 : +1;
        mp->sweep_dwell_until_ms = 0;
        break;

    default:
        break;
    }

    mp->id  = id;
    mp->t_s = 0.0f;
}

void motion_profile_set_manual(motion_profile_t *mp, float yaw_deg, float pitch_deg)
{
    mp->manual_yaw_deg   = yaw_deg;
    mp->manual_pitch_deg = pitch_deg;
    mp->center_yaw_deg   = yaw_deg;
    mp->center_pitch_deg = pitch_deg;
}

void motion_profile_step(motion_profile_t *mp, float dt_s, ctrl_target_t *out)
{
    if (dt_s < 0.0f) {
        dt_s = 0.0f;
    } else if (dt_s > 0.5f) {
        dt_s = 0.5f;   /* 防止卡顿后轨迹跳变 */
    }
    mp->t_s += dt_s;

    out->yaw_deg         = 0.0f;
    out->pitch_deg       = 0.0f;
    out->yaw_rate_dps    = 0.0f;
    out->pitch_rate_dps  = 0.0f;

    switch (mp->id) {

    case PROFILE_STATIC:
        out->yaw_deg   = mp->manual_yaw_deg;
        out->pitch_deg = mp->manual_pitch_deg;
        break;

    case PROFILE_MANUAL:
        /* 手动模式下由串口命令直接写角度, 这里按限速平滑逼近, 避免阶跃 */
        {
            const float max_step = SERIAL_MANUAL_MAX_DPS_LOCAL * dt_s;
            float dy = mp->manual_yaw_deg   - mp->last_yaw_deg;
            float dp = mp->manual_pitch_deg - mp->last_pitch_deg;
            if (dy >  max_step) dy =  max_step;
            if (dy < -max_step) dy = -max_step;
            if (dp >  max_step) dp =  max_step;
            if (dp < -max_step) dp = -max_step;
            mp->last_yaw_deg   += dy;
            mp->last_pitch_deg += dp;
            out->yaw_deg       = mp->last_yaw_deg;
            out->pitch_deg     = mp->last_pitch_deg;
            out->yaw_rate_dps  = (dt_s > 0.0f) ? (dy / dt_s) : 0.0f;
            out->pitch_rate_dps= (dt_s > 0.0f) ? (dp / dt_s) : 0.0f;
        }
        break;

    case PROFILE_SINE:
        {
            float w = TWO_PI / mp->sine_period_s;         /* rad/s */
            float a = mp->sine_amp_deg;
            out->yaw_deg      = mp->center_yaw_deg + a * sinf(w * mp->t_s);
            out->yaw_rate_dps = a * w * cosf(w * mp->t_s); /* 精确导数, 无滞后 */
            out->pitch_deg    = mp->center_pitch_deg;
        }
        break;

    case PROFILE_SWEEP:
        {
            uint32_t now = (uint32_t)(mp->t_s * 1000.0f);

            if (mp->sweep_dwell_until_ms != 0 && now < mp->sweep_dwell_until_ms) {
                /* 端点停留 */
                out->yaw_deg      = mp->sweep_pos_deg;
                out->yaw_rate_dps = 0.0f;
            } else {
                mp->sweep_dwell_until_ms = 0;
                mp->sweep_pos_deg += (float)mp->sweep_dir * mp->sweep_speed_dps * dt_s;

                if (mp->sweep_pos_deg >= mp->sweep_amp_deg) {
                    mp->sweep_pos_deg = mp->sweep_amp_deg;
                    mp->sweep_dir = -1;
                    mp->sweep_dwell_until_ms = now + mp->sweep_dwell_ms;
                } else if (mp->sweep_pos_deg <= -mp->sweep_amp_deg) {
                    mp->sweep_pos_deg = -mp->sweep_amp_deg;
                    mp->sweep_dir = +1;
                    mp->sweep_dwell_until_ms = now + mp->sweep_dwell_ms;
                }
                out->yaw_deg      = mp->sweep_pos_deg;
                out->yaw_rate_dps = (float)mp->sweep_dir * mp->sweep_speed_dps;
            }
            out->pitch_deg = mp->center_pitch_deg;
        }
        break;

    case PROFILE_CIRCLE:
        {
            float w = TWO_PI / mp->circle_period_s;
            float r = mp->circle_radius_deg;
            out->yaw_deg         = mp->center_yaw_deg   + r * cosf(w * mp->t_s);
            out->pitch_deg       = mp->center_pitch_deg + r * sinf(w * mp->t_s);
            out->yaw_rate_dps    = -r * w * sinf(w * mp->t_s);
            out->pitch_rate_dps  =  r * w * cosf(w * mp->t_s);
        }
        break;

    case PROFILE_FIGURE8:
        {
            float w = TWO_PI / mp->fig8_period_s;
            float ax = mp->fig8_yaw_amp_deg;
            float ay = mp->fig8_pitch_amp_deg;
            out->yaw_deg         = mp->center_yaw_deg   + ax * sinf(w * mp->t_s);
            out->pitch_deg       = mp->center_pitch_deg + ay * sinf(2.0f * w * mp->t_s);
            out->yaw_rate_dps    =  ax * w * cosf(w * mp->t_s);
            out->pitch_rate_dps  =  ay * 2.0f * w * cosf(2.0f * w * mp->t_s);
        }
        break;

    default:
        break;
    }

    clamp_target(out);

    /* 记录上一帧, 供手动模式做速率限制 */
    mp->last_yaw_deg   = out->yaw_deg;
    mp->last_pitch_deg = out->pitch_deg;
}

const char *motion_profile_name(profile_id_t id)
{
    switch (id) {
    case PROFILE_STATIC:  return "STATIC";
    case PROFILE_SINE:    return "SINE";
    case PROFILE_SWEEP:   return "SWEEP";
    case PROFILE_CIRCLE:  return "CIRCLE";
    case PROFILE_FIGURE8: return "FIGURE8";
    case PROFILE_MANUAL:  return "MANUAL";
    default:              return "?";
    }
}
