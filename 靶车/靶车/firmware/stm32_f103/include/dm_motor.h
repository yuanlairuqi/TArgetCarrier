/**
 * @file    dm_motor.h
 * @brief   达妙 DM 系列电机 MIT 模式协议 (CAN)
 *
 *  协议来源: 达妙官方例程 dm_motor_drv.c / .h (资料包 控制例程 内),
 *           字节打包规则逐位对照实现, 没有改动。
 *
 *  CAN ID 规则 (电机 ID = 1 为例):
 *    MIT 模式控制帧     0x000 + id  ->  0x001
 *    位置速度模式       0x100 + id  ->  0x101
 *    速度模式           0x200 + id  ->  0x201
 *    位置速度电流模式   0x300 + id  ->  0x301
 *    使能               id + 模式ID, 数据 FF FF FF FF FF FF FF FC
 *    失能               id + 模式ID, 数据 FF FF FF FF FF FF FF FD
 *    保存零点           id + 模式ID, 数据 FF FF FF FF FF FF FF FE
 *    清除错误           id + 模式ID, 数据 FF FF FF FF FF FF FF FB
 *
 *  反馈帧 (电机 -> 主控):
 *    data[0] = (错误码 << 4) | 电机ID
 *    data[1..2] = 位置 (16bit 无符号)
 *    data[3..4] = 速度 (12bit)      data[4] 低 4 位 + data[5] = 力矩 (12bit)
 *    data[6] = MOS 温度   data[7] = 线圈温度
 */

#ifndef DM_MOTOR_H
#define DM_MOTOR_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DM_MODE_MIT = 0x000,
    DM_MODE_POS = 0x100,
    DM_MODE_SPD = 0x200,
    DM_MODE_PSI = 0x300,
} dm_mode_t;

/* MIT 控制指令 (物理单位: 弧度 / 弧度每秒 / 牛米) */
typedef struct {
    float pos_rad;
    float vel_rad_s;
    float kp;
    float kd;
    float torque_nm;
} dm_mit_cmd_t;

/* 电机反馈 (物理单位) */
typedef struct {
    uint8_t id;
    uint8_t err;          /* 0 = 正常, 非 0 见达妙手册错误码表 */
    float   pos_rad;
    float   vel_rad_s;
    float   torque_nm;
    uint8_t temp_mos;
    uint8_t temp_coil;
} dm_feedback_t;

/* ---- 编解码 (纯函数, 便于主机端单元测试) ---- */

/** 把浮点映射成无符号整数 (达妙 MIT 线性映射) */
uint16_t dm_float_to_uint(float x, float x_min, float x_max, uint8_t bits);

/** 把无符号整数还原成浮点 */
float dm_uint_to_float(uint16_t x, float x_min, float x_max, uint8_t bits);

/**
 * @brief 打包 MIT 控制帧
 * @param id 电机 CAN ID
 * @param out 8 字节输出
 * @return CAN 标准帧 ID (0x000 + id)
 */
uint32_t dm_pack_mit(uint8_t id, const dm_mit_cmd_t *cmd, uint8_t *out);

/**
 * @brief 解析电机反馈帧
 * @return true = 长度与 ID 匹配, 解析有效
 */
bool dm_parse_feedback(uint8_t motor_id, const uint8_t *data, uint8_t len,
                       dm_feedback_t *fb);

/** 打包"特殊命令" (使能/失能/清错/存零点), 返回 CAN ID */
uint32_t dm_pack_special(uint8_t id, dm_mode_t mode, uint8_t cmd_byte, uint8_t *out);

/* 特殊命令字节 (对应数据末字节) */
#define DM_CMD_ENABLE     0xFCu
#define DM_CMD_DISABLE    0xFDu
#define DM_CMD_SAVE_ZERO  0xFEu
#define DM_CMD_CLEAR_ERR  0xFBu

#ifdef __cplusplus
}
#endif

#endif /* DM_MOTOR_H */
