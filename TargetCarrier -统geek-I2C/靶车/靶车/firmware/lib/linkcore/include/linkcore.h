/**
 * @file    linkcore.h
 * @brief   靶车三端共用的"链路可靠性内核" —— 纯 C, 不依赖 RTOS / HAL / Arduino
 *
 *  为什么单独抽一层:
 *   序列号回绕、命令过期、ARM 授权、反馈新鲜度、ESP-NOW 提交/回调统计、
 *   CAN 邮箱优先级 —— 这些逻辑决定"会不会意外恢复运动", 但都跟具体外设
 *   无关。抽成纯 C 后可以在主机上真跑测试 (lib/linkcore/test), 而不是只靠
 *   肉眼审查; 三端固件只保留很薄的适配层 (读 DMA / 写邮箱 / 发 ESP-NOW)。
 *
 *  约定:
 *    - 所有时间参数都是"同一时钟源的毫秒数", 只用无符号差值比较, 天然抗回绕。
 *    - 全部静态分配, 无 malloc, 无阻塞, 无全局状态 (调用者持有上下文)。
 *    - 本文件不打印、不开关中断; 临界区由调用者按平台提供。
 */

#ifndef LINCORE_H
#define LINCORE_H

#include <stdint.h>
#include <stdbool.h>

#include "protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* 1. 8 bit 序号比较 (RFC1982 风格, 窗口 128)                          */
/* ================================================================== */
/* 50 Hz 下 256 帧 = 5.12 s 才回绕一次, 窗口 128 = 2.56 s, 足够。
 * 返回 1 表示 a 比 b 新, -1 表示旧, 0 表示相同 (重复帧)。 */
static inline int lc_seq_cmp8(uint8_t a, uint8_t b)
{
    uint8_t d = (uint8_t)(a - b);
    if (d == 0u) {
        return 0;
    }
    return (d < 0x80u) ? 1 : -1;
}

typedef struct {
    uint8_t  last;
    bool     have;
    uint32_t accepted;
    uint32_t duplicate;
    uint32_t reorder;
} lc_seq_gate_t;

void lc_seq_gate_init(lc_seq_gate_t *g);

/**
 * @brief 过滤重复/乱序/回绕帧
 * @return true = 这是一帧"更新的"命令, 应继续处理
 */
bool lc_seq_accept(lc_seq_gate_t *g, uint8_t seq);

/* ================================================================== */
/* 2. 时间戳与命令有效期                                               */
/* ================================================================== */

/** 无符号差值, 处理 32 位毫秒回绕 (49.7 天) */
static inline uint32_t lc_elapsed_ms(uint32_t now_ms, uint32_t then_ms)
{
    return (uint32_t)(now_ms - then_ms);
}

static inline bool lc_age_exceeds(uint32_t now_ms, uint32_t rx_ms, uint32_t limit_ms)
{
    return lc_elapsed_ms(now_ms, rx_ms) > limit_ms;
}

/* ================================================================== */
/* 3. ARM 授权状态机 —— STM32 是权威判定方                             */
/* ================================================================== */
/* 语义:
 *   - 复位后必须先见到一帧 ENABLE=0/ARM=0 的帧 (boot_disarm_seen), 之后才允许授权。
 *     这是为了挡住"STM32 复位/掉电重启时, 主机仍在发复位前的 ENABLE|ARM|旧令牌":
 *     单片机重启只需几十毫秒, 主机 300ms 的失联判定可能还没触发, 旧令牌会被
 *     last_token=0 当成"新授权"。有了启动门, 复位后必须等主机先发一次失能,
 *     才会重新接受授权。
 *   - 只有 ENABLE + ARM + 非零令牌, 且令牌 != 上次已接受令牌, 才进入 armed。
 *   - 任何不带 ARM 的帧、带 ESTOP 的帧、或者调用 lc_arm_force_safe() 都会撤销 armed。
 *   - 撤销后即使主机继续发同一个旧令牌, 也不会重新 armed (防重放/防断联自动恢复)。 */
typedef struct {
    uint8_t  last_token;   /* 上次已接受令牌, 0 = 从未接受 */
    bool     armed;
    bool     boot_disarm_seen; /* 复位后必须先见到一帧"未使能"才允许授权 */
    uint32_t arm_ok;       /* 接受次数 */
    uint32_t arm_reject;   /* 拒绝次数 (旧令牌/重放/缺 ARM/复位后未见失能) */
    uint32_t disarm;       /* 由帧撤销的次数 */
    uint32_t safe_events;  /* force_safe 次数 */
} lc_arm_t;

void lc_arm_init(lc_arm_t *a);

/**
 * @brief 每条控制帧调用一次, 返回本帧之后是否允许运动
 */
bool lc_arm_update(lc_arm_t *a, uint16_t flags);

/** 急停 / 故障 / 断联 / 复位 / CAN 恢复 / 电机反馈超时 —— 一律撤销授权 */
void lc_arm_force_safe(lc_arm_t *a);

static inline bool lc_arm_armed(const lc_arm_t *a)
{
    return a != NULL && a->armed;
}

static inline uint8_t lc_arm_token(const lc_arm_t *a)
{
    return (a != NULL) ? a->last_token : 0u;
}

/* ================================================================== */
/* 4. 授权产生侧 (ESP32 主机)                                          */
/* ================================================================== */
typedef struct {
    uint8_t  token;
    bool     active;
    uint32_t issued;
} lc_arm_tx_t;

/** seed 用 boot 计数/随机数, 保证复位后令牌与上次不同 */
void lc_arm_tx_init(lc_arm_tx_t *t, uint8_t seed);
/** 用户明确点"使能"时调用: 生成新的非零令牌 */
uint8_t lc_arm_tx_next(lc_arm_tx_t *t);
/** 失能/急停/断联时调用: 本端也立刻失效, 不再发 ARM */
void lc_arm_tx_clear(lc_arm_tx_t *t);
static inline bool lc_arm_tx_active(const lc_arm_tx_t *t) { return t != NULL && t->active; }

/* ================================================================== */
/* 5. 反馈/链路新鲜度                                                  */
/* ================================================================== */
typedef struct {
    uint32_t last_ms;
    bool     have;
    uint32_t updates;
} lc_fresh_t;

void lc_fresh_init(lc_fresh_t *f);
void lc_fresh_kick(lc_fresh_t *f, uint32_t now_ms);
bool lc_fresh_ok(const lc_fresh_t *f, uint32_t now_ms, uint32_t timeout_ms);
uint32_t lc_fresh_age(const lc_fresh_t *f, uint32_t now_ms, bool *have);

/* ================================================================== */
/* 6. ESP-NOW 发送统计: 提交 / 异步回调 / 应用层确认 三者分离           */
/* ================================================================== */
/* ESP-NOW v1 的发送回调只带 status, 不带用户上下文, 回调顺序与提交顺序
 * 一致。旧实现把"上一帧的回调结果"算到"当前帧"头上, 统计永久错位一帧,
 * 不同报文 (控制/增益/参数/上行) 还互相串扰。这里用提交队列把每一帧
 * 的结果按类型分开记账。 */
typedef enum {
    LC_TX_CTRL = 0,     /* 控制帧 */
    LC_TX_UPLINK = 1,   /* 桥接上行状态 */
    LC_TX_PARAM = 2,    /* 参数/增益帧 */
    LC_TX_KIND_N = 3
} lc_tx_kind_t;

#define LC_TX_INFLIGHT_MAX 8u

typedef struct {
    uint8_t  kind[LC_TX_INFLIGHT_MAX];
    uint16_t tag[LC_TX_INFLIGHT_MAX];
    uint32_t ms[LC_TX_INFLIGHT_MAX];
    uint8_t  head;
    uint8_t  count;

    uint32_t submit_ok[LC_TX_KIND_N];
    uint32_t submit_fail[LC_TX_KIND_N];
    uint32_t cb_ok[LC_TX_KIND_N];
    uint32_t cb_fail[LC_TX_KIND_N];
    uint32_t confirm[LC_TX_KIND_N];   /* 应用层确认 (如 STM32 回显令牌/状态帧) */
    bool     cb_desynced;             /* Missing callback: subsequent results cannot be correlated. */
    uint32_t cb_timeout;              /* 在途超时 */
    uint32_t cb_unexpected;           /* 没有在途却收到回调 */
    uint32_t dropped_inflight_full;   /* 在途表满, 提交未记账 */
} lc_txstats_t;

void lc_txstats_init(lc_txstats_t *s);

/**
 * @brief 记录一次发送提交
 * @param submit_ok esp_now_send() 是否返回 ESP_OK (仅代表入队成功)
 * @return true = 已记入在途表, 之后会对应一次回调
 */
bool lc_txstats_submit(lc_txstats_t *s, lc_tx_kind_t kind, uint16_t tag,
                       uint32_t now_ms, bool submit_ok);

/** 每次 ESP-NOW 发送回调调用一次 (按提交顺序出队) */
void lc_txstats_cb(lc_txstats_t *s, bool ok, uint32_t now_ms);

/** 周期性调用: 把超过 timeout 的在途帧记为超时 (返回本次新超时数) */
uint32_t lc_txstats_expire(lc_txstats_t *s, uint32_t now_ms, uint32_t timeout_ms);

void lc_txstats_confirm(lc_txstats_t *s, lc_tx_kind_t kind);

/* Exact command recognition shared by firmware and regression tests. */
typedef enum {
    LC_STOP_NONE = 0, LC_STOP_DISABLE = 1, LC_STOP_ESTOP = 2,
    LC_STOP_RELEASE = 3, LC_STOP_YAW = 4, LC_STOP_PITCH = 5, LC_STOP_CRUISE = 6
} lc_stop_command_t;
lc_stop_command_t lc_stop_command(const char *line);

static inline uint8_t lc_txstats_inflight(const lc_txstats_t *s)
{
    return s->count;
}

/* ================================================================== */
/* 7. CAN 发送协调策略 (主循环任务与恢复流程共用同一入口)               */
/* ================================================================== */
/* 目标:
 *   - 安全命令 (失能/使能/清错/存零点) 有独立优先级, 不因邮箱满而丢;
 *   - 运动帧只保留"最新一帧", 不排队重放旧目标;
 *   - 失能永远压过使能: 停止后队列里的旧 ENABLE 必须作废;
 *   - 统计"已提交 / 总线完成 / 失败", 不把提交当成功。 */
typedef enum {
    LC_CAN_SAFETY_DISABLE = 0,   /* 最高: 失能/急停 */
    LC_CAN_SAFETY_ENABLE  = 1,   /* 使能 (请求 DISABLE 会清掉它) */
    LC_CAN_SAFETY_CLEAR   = 2,   /* 清错 */
    LC_CAN_SAFETY_ZERO    = 3,   /* 存零点 */
    LC_CAN_SAFETY_N       = 4
} lc_can_safety_t;

typedef enum {
    LC_CAN_PRIO_SAFETY = 0,
    LC_CAN_PRIO_MOTION = 1,
    LC_CAN_PRIO_PARAM  = 2
} lc_can_prio_t;

typedef struct {
    uint32_t id;
    uint8_t  data[8];
    uint8_t  len;
    uint8_t  prio;    /* lc_can_prio_t */
    uint8_t  tag;     /* 安全类: lc_can_safety_t; 其它: 自定义 */
} lc_can_frame_t;

#define LC_CAN_PARAM_QMAX 2u

typedef struct {
    lc_can_frame_t safety[LC_CAN_SAFETY_N];
    bool           safety_pending[LC_CAN_SAFETY_N];
    uint32_t       safety_seq[LC_CAN_SAFETY_N];
    uint32_t       safety_clock;

    lc_can_frame_t motion;
    bool           have_motion;

    lc_can_frame_t param[LC_CAN_PARAM_QMAX];
    uint8_t        n_param;

    uint32_t submitted;         /* 成功写入邮箱 */
    uint32_t submit_fail;       /* 硬件邮箱满/出错 */
    uint32_t motion_coalesced;  /* 被新目标覆盖的旧运动帧 */
    uint32_t safety_dropped;    /* 安全槽被覆盖/丢弃 (仅同类替换) */
    uint32_t param_dropped;     /* 参数队列满丢弃 */
    uint32_t bus_ok;            /* 邮箱完成且总线 ACK (由适配层上报) */
    uint32_t bus_fail;
    uint32_t recoveries;        /* CAN 重新初始化次数 */
} lc_can_tx_t;

void lc_can_tx_init(lc_can_tx_t *t);

/** 提交一帧; 返回 false 表示被丢弃 (仅参数类会因队列满返回 false) */
bool lc_can_tx_request(lc_can_tx_t *t, const lc_can_frame_t *f);

/** 是否有待发送内容 */
bool lc_can_tx_pending(const lc_can_tx_t *t);

/** 取下一帧应该尝试写入邮箱的帧 (按优先级), 不改状态 */
const lc_can_frame_t *lc_can_tx_peek(const lc_can_tx_t *t);

/**
 * @brief 已尝试写入一帧后回报结果
 * @param written 底层 can_drv_send() 是否成功写入空闲邮箱
 */
void lc_can_tx_pop(lc_can_tx_t *t, bool written);

/** 底层上报邮箱完成结果 (RQCP + TXOK; TME alone is not success). */
void lc_can_tx_report_bus(lc_can_tx_t *t, bool ok);

/** CAN 总线重新初始化: 丢弃运动/参数帧, 安全帧保留重发 */
void lc_can_tx_abort(lc_can_tx_t *t);

/** 有新安全命令时清掉"旧运动目标", 防止停止后继续跑 (停止屏障) */
void lc_can_tx_clear_motion(lc_can_tx_t *t);

/* ================================================================== */
/* 8. 控制命令队列 (link 任务 -> 控制任务)                              */
/* ================================================================== */
/* 安全命令有保留槽位; 运动命令队列满时丢旧帧并计数; 停止后清掉旧运动帧;
 * 消费时按"安全优先 + 过期丢弃"出队。 */
typedef struct {
    uint16_t flags;
    uint8_t  seq;
    uint8_t  mode;
    int16_t  yaw_q;
    int16_t  pitch_q;
    int16_t  yawrate_q;
    int16_t  pitchrate_q;
    uint32_t rx_ms;
    uint8_t  safety;   /* 1 = 安全类命令 */
} lc_cmd_t;

#define LC_CMD_QMAX 6u
#define LC_CMD_SAFETY_RESERVED 2u

typedef struct {
    lc_cmd_t q[LC_CMD_QMAX];
    uint8_t  n;
    uint32_t push_ok;
    uint32_t push_drop_motion;
    uint32_t push_drop_safety;
    uint32_t expired;
    uint32_t purged_motion;
} lc_cmdq_t;

void lc_cmdq_init(lc_cmdq_t *q);

/** 入队; 安全命令优先占保留槽, 运动命令满则丢最旧的运动命令 */
bool lc_cmdq_push(lc_cmdq_t *q, const lc_cmd_t *cmd);

/** 删除所有排队中的运动命令 (停止/急停屏障) */
uint32_t lc_cmdq_purge_motion(lc_cmdq_t *q);

void lc_cmdq_reset(lc_cmdq_t *q);

/**
 * @brief 取下一帧要执行的命令: 安全优先, 过期/重复序号丢弃
 * @param seq_gate 传 NULL 表示不做序号过滤
 */
bool lc_cmdq_pop(lc_cmdq_t *q, uint32_t now_ms, uint32_t max_age_ms,
                 lc_seq_gate_t *seq_gate, lc_cmd_t *out);

/* ================================================================== */
/* 9. 字节流成帧器 (UART / DMA 消费)                                   */
/* ================================================================== */
typedef enum {
    LC_RX_NONE = 0,
    LC_RX_CTRL,
    LC_RX_GAINS,
    LC_RX_PARAM,
    LC_RX_STATUS,
    LC_RX_MOTOR,
    LC_RX_PARAM_REPLY,
    LC_RX_BRIDGE_STATUS,
    LC_RX_UNKNOWN
} lc_rx_kind_t;

typedef struct {
    uint8_t  buf[17];
    uint8_t  len;
    uint32_t frames_ok;
    uint32_t frames_bad;
    uint32_t crc_bad;
    uint32_t resync;
    uint32_t overflow;
    uint32_t last_valid_ms;
    bool     have_valid;
} lc_rx_t;

void lc_rx_init(lc_rx_t *rx);

/**
 * @brief 喂一个字节; 成帧时返回类型并拷贝 17 字节到 out
 *        (OUT 可为 NULL, 此时只做校验/统计)
 */
lc_rx_kind_t lc_rx_feed(lc_rx_t *rx, uint8_t byte, uint32_t now_ms, uint8_t out[17]);

/** DMA 环形缓冲溢出/未消费积压后调用: 丢掉半帧, 等下一次帧头重新同步 */
void lc_rx_overflow(lc_rx_t *rx);

#ifdef __cplusplus
}
#endif

#endif /* LINCORE_H */
