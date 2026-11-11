/**
 * @file    param_store.h
 * @brief   靶车 —— 主机参数掉电保存 (NVS / Flash)
 *
 *  保存的是"可调参数", 不是运行状态:
 *      kp / kd、正弦幅度与周期、扫描幅度与角速度、圆周半径
 *  ★ 故意不保存 使能位 / 急停 / 轨迹模式:
 *    上电必须保持"失能 + 定点", 否则一上电云台就自己动, 太危险。
 */

#ifndef PARAM_STORE_H
#define PARAM_STORE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 存进去的魔数: 结构体一旦改字段, 就改这个数, 旧数据会被丢弃而不是被误读 */
#define PARAM_STORE_MAGIC   0x54435232u   /* "TCR2" */

typedef struct {
    uint32_t magic;
    float    kp;
    float    kd;
    float    sine_amp_deg;
    float    sine_period_s;
    float    sweep_amp_deg;
    float    sweep_speed_dps;
    float    circle_radius_deg;
} param_blob_t;

/** 从 Flash 读一组参数; 没有存过/版本不匹配则返回 false */
bool param_store_load(param_blob_t *out);

/** 把参数写入 Flash (会 commit, 大约几毫秒) */
bool param_store_save(const param_blob_t *in);

/* ------------------------------------------------------------------ */
/* 参数预设槽位 (存/取/删 一组参数, 带名字)                            */
/* ------------------------------------------------------------------ */

#define PRESET_SLOTS      4
#define PRESET_NAME_LEN   16

typedef struct {
    uint32_t      magic;
    char          name[PRESET_NAME_LEN];
    param_blob_t  params;
} preset_blob_t;

/** 保存当前参数到第 slot 槽 (0..PRESET_SLOTS-1); name 可为空 */
bool preset_save(uint8_t slot, const char *name, const param_blob_t *params);

/** 读第 slot 槽; 空槽返回 false */
bool preset_load(uint8_t slot, preset_blob_t *out);

/** 清空第 slot 槽 */
bool preset_clear(uint8_t slot);

#ifdef __cplusplus
}
#endif

#endif /* PARAM_STORE_H */
