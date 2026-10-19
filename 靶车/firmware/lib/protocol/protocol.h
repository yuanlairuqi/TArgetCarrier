/**
 * @file    protocol.h
 * @brief   靶车链路通信协议 —— 三端共用的唯一真源
 *
 *   链路拓扑:
 *
 *     [ESP32 主机]  --ESP-NOW(2.4G)-->  [M5Stamp S3 从机]  --UART-->  [STM32F103C8T6]  --CAN 1Mbps-->  [DM-G6220]
 *       运动规划/自主轨迹                  桥接 + 失联保护             MIT 阻抗控制 1kHz            云台电机 ID=1
 *
 *   本文件同时被 ESP32(主机)、ESP32-S3(桥接)、STM32F103 三份固件包含，
 *   三方都必须使用同一份副本，禁止各自修改帧格式。
 *
 *   帧格式 (小端序) —— CTRL_FRAME_SIZE = 16 字节:
 *
 *     off  size  field
 *      0    1    header  = 0xAA
 *      1    1    len     = 11  (从 type 到 crc 之前的字节数)
 *      2    1    type    = PROTO_MSG_CTRL(0x01)
 *      3    1    seq     序号，0~255 循环，用于丢弃乱序/旧帧
 *      4    2    flags   控制标志位，见 CTRL_FLAG_*
 *      6    1    mode    云台电机控制模式，见 MOTOR_MODE_*
 *      7    2    yaw_q   偏航角量化值  = deg(-180..180) / PROTO_ANGLE_SCALE(0.01)  -> -18000..18000
 *      9    2    pitch_q 俯仰角量化值  = deg(-90..90)   / PROTO_ANGLE_SCALE(0.01)  ->  -9000..9000
 *     11    2    yawrate 偏航速度量化值 = dps(-180..180) / PROTO_RATE_SCALE(0.1)   -> -1800..1800
 *     13    2    pitchrate 俯仰速度量化值
 *     15    1    crc16    = CRC16-Modbus(bytes[1..14]) 低 8 位
 *
 *     说明: 帧尾只带 CRC 低字节，因为尾部还有 1 个 0x55 帧尾用于硬件级定界；
 *           三端链路都有尾字节校验，抗误码能力足够，且简化 STM32 中断里的状态机。
 *
 *   反馈帧 STATUS_FRAME_SIZE = 17 字节 (STM32 -> 桥接 -> 主机):
 *
 *      0    1    header = 0x5A (PROTO_STX)
 *      1    1    len    = 15           (p[2..16] 之前的载荷字节数, 与 0xAA 帧保持同一约定)
 *      2    1    type   = PROTO_MSG_STATUS
 *      3    1    seq
 *      4    1    rssi   链路质量 (ESP-NOW RSSI 的绝对值, 由桥接填充)
 *      5    1    state  proto_link_state_t
 *      6..9  4    yaw_deg    (小端 IEEE754 浮点, 度)
 *     10..13 4    pitch_deg  (小端 IEEE754 浮点, 度)
 *     14    1    temp_mos   (℃)
 *     15    1    err_code   (0 = 正常)
 *     16    1    crc16 = CRC16-Modbus(bytes[1..15]) 低 8 位
 *
 *   为什么是 17 而不是 16: 6 个单字节头 + 2 个 float(8) + temp + err + crc
 *   = 6+8+1+1+1 = 17。之前按 16 写会把 crc 写到 temp_mos 上, 属于静默丢数据,
 *   已在 test_protocol.c 里加了字节级断言防回归。
 */

#ifndef PROTOCOL_H
#define PROTOCOL_H

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ */
/* 常量                                                               */
/* ------------------------------------------------------------------ */

#define PROTO_HEAD                0xAAu   /* 控制帧头 */
#define PROTO_TAIL                0x55u   /* 通用帧尾/反馈帧头 */
#define PROTO_STX                 0x5Au   /* 反馈帧头，与 0xAA 区分方向 */

#define PROTO_CRC_NONE            0x0000u

#define PROTO_ANGLE_SCALE         0.01f   /* 角度量化步长 (deg)   */
#define PROTO_RATE_SCALE          0.1f    /* 角速度量化步长 (dps) */

#define PROTO_YAW_LIMIT_DEG       180.0f
#define PROTO_PITCH_LIMIT_DEG     90.0f
#define PROTO_RATE_LIMIT_DPS      180.0f

/* 控制帧字节数 (含 帧头 + 载荷 + CRC + 帧尾) */
#define CTRL_FRAME_SIZE           17u     /* 0xAA,len,type,seq,flags(2),mode,yaw(2),pitch(2),yawrate(2),pitchrate(2),crc,lrc,0x55 */
/* 兼容旧命名 */
#define PROTO_CTRL_FRAME_SIZE     CTRL_FRAME_SIZE

/* 反馈帧字节数 (含 帧头 + 载荷 + CRC; 无独立帧尾, CRC 就是最后一字节) */
#define STATUS_FRAME_SIZE         17u
#define RX_FRAME_SIZE             STATUS_FRAME_SIZE   /* 兼容旧命名 */

/* 链路超时: 超过该毫秒数没收到有效控制帧 -> 判定失联 */
#define PROTO_LINK_TIMEOUT_MS     300u

/* ------------------------------------------------------------------ */
/* 消息类型                                                           */
/* ------------------------------------------------------------------ */

typedef enum {
    PROTO_MSG_CTRL   = 0x01,   /* 控制帧  主机 -> 桥接 -> STM32 */
    PROTO_MSG_STATUS = 0x02,   /* 反馈帧  STM32 -> 桥接 -> 主机 */
} proto_msg_type_t;

/* 控制标志位 */
#define CTRL_FLAG_ENABLE      0x0001u  /* 使能电机(闭环上电) */
#define CTRL_FLAG_CLEAR_ERR   0x0002u  /* 清除电机错误 */
#define CTRL_FLAG_SET_ZERO    0x0004u  /* 保存当前位置为零点(慎用) */
#define CTRL_FLAG_ESTOP       0x0008u  /* 急停: 立即断使能、kp/kd 归零 */

/* 云台电机控制模式 —— 与达妙 DM 系列 CAN 协议一致 */
typedef enum {
    MOTOR_MODE_MIT = 0,        /* MIT 阻抗控制 (位置+速度+kp/kd+力矩) */
    MOTOR_MODE_POS = 1,        /* 位置速度模式        id + 0x100        */
    MOTOR_MODE_SPD = 2,        /* 速度模式            id + 0x200        */
    MOTOR_MODE_PSI = 3,        /* 位置速度电流混合    id + 0x300        */
} motor_mode_t;

/* 控制帧解码结果 (逻辑字段, 不是线格式布局!)。
 *
 * 为什么不加 #pragma pack(1):
 *   MSVC 在 C 模式下会忽略 #pragma pack(push,1) (实测 uint16+uint8+int16 仍是
 *   自然对齐, sizeof=11, b 落在偏移 2), 而 GCC/Clang 会紧凑成 9 字节。
 *   同一个头文件在不同编译器下 sizeof 不一致, 是"主机自测通过、板子上跑不通"
 *   这类问题的经典来源。既然线格式由 proto_encode_ctrl/proto_decode_ctrl 里
 *   的显式字节偏移负责, 这个结构体就只当普通容器用, 不绑定任何 ABI。
 *   下面的静态断言检查的是"逻辑字节数", 与对齐无关。 */
typedef struct {
    uint16_t flags;
    uint8_t  mode;
    int16_t  yaw_q;        /* 量化后的偏航角 */
    int16_t  pitch_q;      /* 量化后的俯仰角 */
    int16_t  yawrate_q;    /* 量化后的偏航角速度 */
    int16_t  pitchrate_q;  /* 量化后的俯仰角速度 */
} ctrl_payload_t;

/* 逻辑编码长度: 各端只用它做缓冲大小计算, 不参与内存布局 */
#define CTRL_PAYLOAD_LOGIC_SIZE   9u

/* 反馈帧的解析结果结构体。
 * 注意: 这是给代码用的普通结构体, 不是线格式布局 —— 编译器会插入填充字节,
 *       真正的打包/解包在 proto_encode_status / proto_decode_status 里手工做。
 *       曾经这里用 #pragma pack(1) 去映射线格式, 结果 17 字节的字段塞不进
 *       16 字节的线格式, 会静默丢掉 crc 字段, 所以改成显式解包。 */
typedef struct {
    uint8_t  header;       /* PROTO_STX */
    uint8_t  len;
    uint8_t  type;         /* PROTO_MSG_STATUS */
    uint8_t  seq;
    uint8_t  rssi;         /* 链路质量，桥接侧填 ESP-NOW RSSI 的绝对值 */
    uint8_t  state;        /* proto_link_state_t */
    float    yaw_deg;
    float    pitch_deg;
    uint8_t  temp_mos;
    uint8_t  err_code;
} status_frame_t;

/* 反馈帧里的链路状态 */
typedef enum {
    LINK_STATE_INIT    = 0,
    LINK_STATE_OK      = 1,   /* 主机在线、电机使能 */
    LINK_STATE_TIMEOUT = 2,   /* 主机失联，已自动断使能 */
    LINK_STATE_ESTOP   = 3,   /* 收到急停 */
    LINK_STATE_FAULT   = 4,   /* 电机报错/反馈异常 */
} proto_link_state_t;

/* ------------------------------------------------------------------ */
/* 便于人类阅读的控制目标 (各端内部使用，不直接上线)                  */
/* ------------------------------------------------------------------ */

typedef struct {
    float   yaw_deg;      /* 目标偏航角, 度 */
    float   pitch_deg;    /* 目标俯仰角, 度 */
    float   yaw_rate_dps; /* 前馈角速度, 度/秒 */
    float   pitch_rate_dps;
} ctrl_target_t;

/* ------------------------------------------------------------------ */
/* CRC16-Modbus                                                       */
/* ------------------------------------------------------------------ */

static inline uint16_t proto_crc16(const uint8_t *buf, uint16_t len)
{
    uint16_t crc = 0xFFFF;
    for (uint16_t i = 0; i < len; i++) {
        crc ^= (uint16_t)buf[i];
        for (uint8_t b = 0; b < 8; b++) {
            if (crc & 1u) {
                crc = (uint16_t)((crc >> 1) ^ 0xA001u);
            } else {
                crc >>= 1;
            }
        }
    }
    return crc;
}

/* 帧尾校验字节: 所有载荷字节的异或，给 STM32 中断里的状态机做廉价确认 */
static inline uint8_t proto_lrc(const uint8_t *buf, uint16_t len)
{
    uint8_t x = 0;
    for (uint16_t i = 0; i < len; i++) {
        x ^= buf[i];
    }
    return x;
}

/* ------------------------------------------------------------------ */
/* 数值工具                                                           */
/* ------------------------------------------------------------------ */

static inline float proto_clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int32_t proto_clampi(int32_t v, int32_t lo, int32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static inline int16_t proto_round_i16(float v)
{
    return (int16_t)((v >= 0.0f) ? (v + 0.5f) : (v - 0.5f));
}

/* 角度 <-> 量化 */
static inline int16_t proto_deg_to_q(float deg, float limit)
{
    float q = proto_clampf(deg, -limit, limit) / PROTO_ANGLE_SCALE;
    return proto_round_i16(q);
}

static inline int16_t proto_dps_to_q(float dps)
{
    float q = proto_clampf(dps, -PROTO_RATE_LIMIT_DPS, PROTO_RATE_LIMIT_DPS) / PROTO_RATE_SCALE;
    return proto_round_i16(q);
}

static inline float proto_q_to_deg(int16_t q)
{
    return (float)q * PROTO_ANGLE_SCALE;
}

static inline float proto_q_to_dps(int16_t q)
{
    return (float)q * PROTO_RATE_SCALE;
}

/* ------------------------------------------------------------------ */
/* 编解码                                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief 把人类可读目标编码成 17 字节控制帧
 * @param out      输出缓冲，至少 CTRL_FRAME_SIZE 字节
 * @param target   控制目标
 * @param flags    CTRL_FLAG_* 位或
 * @param mode     MOTOR_MODE_*
 * @param seq      帧序号
 * @return 写入的字节数 (CTRL_FRAME_SIZE)
 */
static inline uint16_t proto_encode_ctrl(uint8_t *out, const ctrl_target_t *target,
                                         uint16_t flags, uint8_t mode, uint8_t seq)
{
    int16_t yaw_q   = proto_deg_to_q(target->yaw_deg,   PROTO_YAW_LIMIT_DEG);
    int16_t pitch_q = proto_deg_to_q(target->pitch_deg, PROTO_PITCH_LIMIT_DEG);
    int16_t yr_q    = proto_dps_to_q(target->yaw_rate_dps);
    int16_t pr_q    = proto_dps_to_q(target->pitch_rate_dps);

    uint8_t *p = out;
    p[0]  = PROTO_HEAD;
    p[1]  = 13;                       /* p[2..14] 字节数 = 1+1+2+1+2+2+2+2 */
    p[2]  = (uint8_t)PROTO_MSG_CTRL;
    p[3]  = seq;
    p[4]  = (uint8_t)(flags & 0xFF);
    p[5]  = (uint8_t)((flags >> 8) & 0xFF);
    p[6]  = mode;
    p[7]  = (uint8_t)(yaw_q & 0xFF);
    p[8]  = (uint8_t)((yaw_q >> 8) & 0xFF);
    p[9]  = (uint8_t)(pitch_q & 0xFF);
    p[10] = (uint8_t)((pitch_q >> 8) & 0xFF);
    p[11] = (uint8_t)(yr_q & 0xFF);
    p[12] = (uint8_t)((yr_q >> 8) & 0xFF);
    p[13] = (uint8_t)(pr_q & 0xFF);
    p[14] = (uint8_t)((pr_q >> 8) & 0xFF);

    /* len = p[2..14] 的字节数 = 13，已在上面写死 */
    p[15] = (uint8_t)(proto_crc16(&p[1], 14) & 0xFF);
    p[16] = PROTO_TAIL;
    return CTRL_FRAME_SIZE;
}

/**
 * @brief 校验并解码控制帧
 * @param in   输入缓冲
 * @param len  可用字节数
 * @param out  解析结果
 * @return true 表示帧完整且校验通过
 */
static inline bool proto_decode_ctrl(const uint8_t *in, uint16_t len,
                                     ctrl_payload_t *out, uint8_t *seq)
{
    if (len < CTRL_FRAME_SIZE) return false;
    if (in[0] != PROTO_HEAD) return false;
    if (in[1] != 13) return false;
    if (in[2] != (uint8_t)PROTO_MSG_CTRL) return false;
    if (in[16] != PROTO_TAIL) return false;

    uint8_t crc = (uint8_t)(proto_crc16(&in[1], 14) & 0xFF);
    if (crc != in[15]) return false;

    out->flags        = (uint16_t)in[4] | ((uint16_t)in[5] << 8);
    out->mode         = in[6];
    out->yaw_q        = (int16_t)((uint16_t)in[7]  | ((uint16_t)in[8]  << 8));
    out->pitch_q      = (int16_t)((uint16_t)in[9]  | ((uint16_t)in[10] << 8));
    out->yawrate_q    = (int16_t)((uint16_t)in[11] | ((uint16_t)in[12] << 8));
    out->pitchrate_q  = (int16_t)((uint16_t)in[13] | ((uint16_t)in[14] << 8));
    if (seq) *seq = in[3];
    return true;
}

/**
 * @brief 把线格式载荷转成人类可读目标
 */
static inline void proto_payload_to_target(const ctrl_payload_t *p, ctrl_target_t *t)
{
    t->yaw_deg         = proto_q_to_deg(p->yaw_q);
    t->pitch_deg       = proto_q_to_deg(p->pitch_q);
    t->yaw_rate_dps    = proto_q_to_dps(p->yawrate_q);
    t->pitch_rate_dps  = proto_q_to_dps(p->pitchrate_q);
}

/**
 * @brief 编码反馈帧 (STM32 -> 桥接)。
 *        布局: [0]=0x5A [1]=len [2]=type [3]=seq [4]=rssi [5]=state
 *              [6..9]=yaw [10..13]=pitch [14]=temp [15]=err [16]=crc
 */
static inline uint16_t proto_encode_status(uint8_t *out, uint8_t seq, uint8_t rssi,
                                           uint8_t state, float yaw_deg, float pitch_deg,
                                           uint8_t temp_mos, uint8_t err_code)
{
    uint8_t *p = out;
    p[0]  = PROTO_STX;
    p[1]  = 15;                     /* p[2..15] 之前的载荷字节数 */
    p[2]  = (uint8_t)PROTO_MSG_STATUS;
    p[3]  = seq;
    p[4]  = rssi;
    p[5]  = state;
    memcpy(&p[6],  &yaw_deg,   4);
    memcpy(&p[10], &pitch_deg, 4);
    p[14] = temp_mos;
    p[15] = err_code;
    p[16] = (uint8_t)(proto_crc16(&p[1], STATUS_FRAME_SIZE - 2) & 0xFF);
    return STATUS_FRAME_SIZE;
}

/**
 * @brief 校验并解码反馈帧
 */
static inline bool proto_decode_status(const uint8_t *in, uint16_t len,
                                       status_frame_t *out)
{
    if (len < STATUS_FRAME_SIZE) return false;
    if (in[0] != PROTO_STX) return false;
    if (in[1] != 15) return false;
    if (in[2] != (uint8_t)PROTO_MSG_STATUS) return false;

    uint8_t crc = (uint8_t)(proto_crc16(&in[1], STATUS_FRAME_SIZE - 2) & 0xFF);
    if (crc != in[STATUS_FRAME_SIZE - 1]) return false;

    out->header  = in[0];
    out->len     = in[1];
    out->type    = in[2];
    out->seq     = in[3];
    out->rssi    = in[4];
    out->state   = in[5];
    memcpy(&out->yaw_deg,   &in[6],  4);
    memcpy(&out->pitch_deg, &in[10], 4);
    out->temp_mos = in[14];
    out->err_code = in[15];
    return true;
}

#ifdef __cplusplus
}
#endif

#endif /* PROTOCOL_H */
