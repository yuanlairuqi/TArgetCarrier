/**
 * @file    test_linkcore.c
 * @brief   链路可靠性内核主机端自测 —— 覆盖真实故障路径, 不需要硬件
 *
 *  覆盖:
 *    1. 协议 v1.1 新增: ARM/令牌 flags、桥接新鲜度帧、电机安全位
 *    2. 序号窗口: 重复 / 乱序 / 迟到 / 8 位回绕
 *    3. 命令队列: 队列满时急停不被丢、停止作废旧启动、过期命令丢弃
 *    4. ARM 授权: 急停解除后不自动恢复、断联恢复后不自动恢复、旧令牌重放被拒
 *    5. 反馈新鲜度与 32 位毫秒回绕
 *    6. ESP-NOW 统计: 提交 / 回调 / 应用确认分离、在途超时、跨类型不串扰
 *    7. CAN 发送协调: 邮箱满不丢安全命令、失能压过使能、停止清运动、恢复重发
 *    8. 字节流成帧: CRC 坏帧后能重新同步、不会把下一帧一起丢掉
 */

#include <stdio.h>
#include <string.h>

#include "protocol.h"
#include "linkcore.h"

static int g_fail = 0;

#define CHECK(cond, msg)                                            \
    do {                                                            \
        if (cond) {                                                 \
            printf("  [PASS] %s\n", msg);                           \
        } else {                                                    \
            printf("  [FAIL] %s  (line %d)\n", msg, __LINE__);      \
            g_fail++;                                               \
        }                                                           \
    } while (0)

/* ================================================================== */
/* 1. 协议新增字段                                                     */
/* ================================================================== */

static void test_protocol_v11(void)
{
    printf("\n== 1. 协议 v1.1 新增字段 ==\n");

    uint8_t f[CTRL_FRAME_SIZE];
    ctrl_target_t t = { 10.0f, -5.0f, 1.0f, 0.0f };
    uint16_t flags = (uint16_t)(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM);
    flags = proto_flags_set_token(flags, 0xA5u);

    proto_encode_ctrl(f, &t, flags, MOTOR_MODE_MIT, 77);

    ctrl_payload_t pl;
    uint8_t seq = 0;
    CHECK(proto_decode_ctrl(f, sizeof(f), &pl, &seq), "带 ARM/令牌的控制帧可解码");
    CHECK(seq == 77u, "序号不变");
    CHECK(pl.flags == flags, "flags 完整往返 (ARM 位+令牌)");
    CHECK(proto_flags_token(pl.flags) == 0xA5u, "令牌解码正确");
    CHECK((pl.flags & CTRL_FLAG_ARM) != 0u, "ARM 位解码正确");
    CHECK(f[0] == 0xAAu && f[1] == 13u && f[16] == 0x55u, "帧头/长度/帧尾不变");
    CHECK(f[15] == (uint8_t)(proto_crc16(&f[1], 14) & 0xFFu), "CRC 覆盖新 flags 位");

    /* proto_frame_requests_arm 语义 */
    uint16_t ok_flags = proto_flags_set_token(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 7);
    CHECK(proto_frame_requests_arm(ok_flags, 6u), "新令牌 => 合法授权请求");
    CHECK(!proto_frame_requests_arm(ok_flags, 7u), "同令牌重放 => 不是新授权");
    CHECK(!proto_frame_requests_arm(CTRL_FLAG_ENABLE, 0u), "只有 ENABLE 没有 ARM => 不是授权");
    CHECK(!proto_frame_requests_arm(proto_flags_set_token(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 0), 0u),
          "令牌 0 => 非法");
    CHECK(!proto_frame_requests_arm(proto_flags_set_token(CTRL_FLAG_ARM, 9), 0u),
          "没有 ENABLE => 不是授权");

    /* 桥接新鲜度帧 */
    uint8_t bs[BRIDGE_STATUS_FRAME_SIZE];
    proto_encode_bridge_status(bs, 3, PROTO_BR_FLAG_STM_FRESH | PROTO_BR_FLAG_FAILSAFE,
                               12, LINK_STATE_ESTOP, 0x08, 200, 5, 2, 199, 4, 9, 1);
    bridge_status_frame_t bst;
    CHECK(proto_decode_bridge_status(bs, sizeof(bs), &bst), "桥接新鲜度帧可解码");
    CHECK(bst.stm_age_10ms == 12u && bst.stm_state == LINK_STATE_ESTOP, "年龄/状态字段");
    CHECK((bst.flags & PROTO_BR_FLAG_STM_FRESH) != 0u, "新鲜标志");
    CHECK(bst.rx_drop_l8 == 2u && bst.arm_reject_l8 == 1u, "丢包/授权拒绝计数");
    bs[5] ^= 0x01u;
    CHECK(!proto_decode_bridge_status(bs, sizeof(bs), &bst), "篡改的新鲜度帧被拒");

    /* 电机扩展帧: 旧解码器仍兼容, 新解码器能读安全位 */
    uint8_t mf[CTRL_FRAME_SIZE];
    proto_encode_motor_ext2(mf, 0.5f, 0.1f, 0.02f, 3u,
                            PROTO_MOTOR_SAFETY_ARMED | PROTO_MOTOR_SAFETY_ENABLED,
                            7u, 0x5Au, 9);
    float p, v, tau;
    uint16_t cf;
    CHECK(proto_decode_motor(mf, sizeof(mf), &p, &v, &tau, &cf), "旧电机帧解码器兼容");
    uint8_t safety, drops, arm_echo;
    CHECK(proto_decode_motor_ext2(mf, sizeof(mf), &p, &v, &tau, &cf,
                                  &safety, &drops, &arm_echo), "扩展电机帧解码");
    CHECK(safety == (PROTO_MOTOR_SAFETY_ARMED | PROTO_MOTOR_SAFETY_ENABLED), "安全位往返");
    CHECK(drops == 7u && arm_echo == 0x5Au, "丢弃计数/令牌回显往返");
}

/* ================================================================== */
/* 2. 序号窗口                                                         */
/* ================================================================== */

static void test_seq_window(void)
{
    printf("\n== 2. 序号窗口 (重复/乱序/迟到/回绕) ==\n");

    lc_seq_gate_t g;
    lc_seq_gate_init(&g);

    CHECK(lc_seq_accept(&g, 10u), "首帧接受");
    CHECK(!lc_seq_accept(&g, 10u), "相邻重复被拒");
    CHECK(lc_seq_accept(&g, 11u), "递增接受");
    CHECK(!lc_seq_accept(&g, 9u), "迟到旧帧被拒");
    CHECK(!lc_seq_accept(&g, 9u), "迟到旧帧再次被拒");
    CHECK(lc_seq_accept(&g, 13u), "跳号(丢包)后新帧接受");
    CHECK(!lc_seq_accept(&g, 12u), "跳号后来迟的 12 被拒");
    CHECK(g.duplicate == 1u, "重复计数");
    CHECK(g.reorder == 3u, "乱序计数");

    /* 8 位回绕: 250 -> 255 -> 0 -> 1 */
    lc_seq_gate_init(&g);
    CHECK(lc_seq_accept(&g, 250u), "回绕前 250");
    CHECK(lc_seq_accept(&g, 255u), "回绕前 255");
    CHECK(lc_seq_accept(&g, 0u), "回绕到 0 被接受");
    CHECK(lc_seq_accept(&g, 1u), "回绕后 1 被接受");
    CHECK(!lc_seq_accept(&g, 255u), "回绕后旧 255 被拒");
    CHECK(lc_seq_cmp8(1u, 255u) > 0, "比较函数: 1 比 255 新");
    CHECK(lc_seq_cmp8(255u, 1u) < 0, "比较函数: 255 比 1 旧");
    CHECK(lc_seq_cmp8(128u, 0u) < 0, "窗口边界: 相差 128 视为旧 (保守)");
}

/* ================================================================== */
/* 3. 命令队列: 队列满 + 急停 + 停止作废旧启动 + 过期                  */
/* ================================================================== */

static lc_cmd_t make_cmd(uint8_t seq, uint32_t rx_ms, bool safety, uint16_t flags)
{
    lc_cmd_t c;
    memset(&c, 0, sizeof(c));
    c.seq = seq;
    c.rx_ms = rx_ms;
    c.safety = safety ? 1u : 0u;
    c.flags = flags;
    return c;
}

static void test_cmd_queue(void)
{
    printf("\n== 3. 命令队列 (队列满/停止屏障/过期) ==\n");

    lc_cmdq_t q;
    lc_cmdq_init(&q);

    for (uint8_t i = 0; i < LC_CMD_QMAX; i++) {
        lc_cmd_t c = make_cmd(i, 1000u + i, false, CTRL_FLAG_ENABLE);
        CHECK(lc_cmdq_push(&q, &c), "运动命令入队");
    }

    /* 队列已满: 普通运动命令应被丢弃, 急停必须能进来 */
    lc_cmd_t motion = make_cmd(100u, 1100u, false, CTRL_FLAG_ENABLE);
    CHECK(!lc_cmdq_push(&q, &motion), "队列满时普通运动命令被丢弃");
    CHECK(q.push_drop_motion == 1u, "丢弃计数");

    lc_cmd_t estop = make_cmd(101u, 1100u, true, CTRL_FLAG_ESTOP);
    CHECK(lc_cmdq_push(&q, &estop), "队列满时急停仍能入队 (挤掉一条运动命令)");

    /* 停止屏障: 清掉排队中的运动命令 */
    lc_cmd_t c1 = make_cmd(102u, 1101u, false, CTRL_FLAG_ENABLE);
    for (int i = 0; i < 3; i++) {
        (void)lc_cmdq_push(&q, &c1);
        c1.seq++;
    }
    uint32_t purged = lc_cmdq_purge_motion(&q);
    CHECK(purged > 0u, "停止后清除了排队中的旧运动命令");
    CHECK(q.purged_motion == purged, "清除计数一致");

    /* 出队: 安全命令优先 */
    lc_cmd_t out;
    CHECK(lc_cmdq_pop(&q, 1150u, 100u, NULL, &out), "出队成功");
    CHECK(out.safety == 1u && out.seq == 101u, "安全命令优先出队");

    /* 过期命令不能再执行: rx 在 1000ms, 现在 5000ms, 有效期 100ms */
    lc_cmdq_init(&q);
    for (uint8_t i = 0; i < 3; i++) {
        lc_cmd_t c = make_cmd((uint8_t)(10u + i), 1000u, false, CTRL_FLAG_ENABLE);
        (void)lc_cmdq_push(&q, &c);
    }
    CHECK(!lc_cmdq_pop(&q, 5000u, 100u, NULL, &out), "过期命令全部丢弃, 一条都不执行");
    CHECK(q.expired == 3u, "过期计数");

    /* 序号过滤: 出队时旧序号不会被当成新命令 */
    lc_cmdq_init(&q);
    lc_seq_gate_t gate;
    lc_seq_gate_init(&gate);
    lc_cmd_t a = make_cmd(20u, 1000u, false, CTRL_FLAG_ENABLE);
    (void)lc_cmdq_push(&q, &a);
    CHECK(lc_cmdq_pop(&q, 1010u, 1000u, &gate, &out), "首帧序号接受");
    lc_cmd_t b = make_cmd(20u, 1010u, false, CTRL_FLAG_ENABLE);
    (void)lc_cmdq_push(&q, &b);
    CHECK(!lc_cmdq_pop(&q, 1020u, 1000u, &gate, &out), "重复序号不执行");
    lc_cmd_t c_old = make_cmd(19u, 1020u, false, CTRL_FLAG_ENABLE);
    (void)lc_cmdq_push(&q, &c_old);
    CHECK(!lc_cmdq_pop(&q, 1030u, 1000u, &gate, &out), "更旧序号不执行");
}

/* ================================================================== */
/* 4. ARM 授权: 断联/急停/故障/重放                                   */
/* ================================================================== */

static uint16_t arm_frame(uint8_t token, bool enable, bool arm, bool estop)
{
    uint16_t f = 0;
    if (enable) f |= CTRL_FLAG_ENABLE;
    if (arm)    f |= CTRL_FLAG_ARM;
    if (estop)  f |= CTRL_FLAG_ESTOP;
    return proto_flags_set_token(f, token);
}

static void test_arm_guard(void)
{
    printf("\n== 4. ARM 授权 (急停/断联/故障/重放) ==\n");

    lc_arm_t a;
    lc_arm_init(&a);
    CHECK(!lc_arm_armed(&a), "上电默认未授权");

    /* 启动门: 复位后主机还没发过"失能"帧时, 就算收到 ENABLE|ARM|令牌 也不能授权。
     * 这是为了挡住"STM32 快速复位 + 主机仍在发复位前使能帧"导致的自动恢复运动。 */
    CHECK(!lc_arm_update(&a, arm_frame(3, true, true, false)),
          "复位后未见失能帧 => 拒绝旧授权帧");
    CHECK(!lc_arm_armed(&a), "启动门未打开时仍未授权");
    CHECK(!lc_arm_update(&a, arm_frame(3, false, false, false)), "失能帧打开启动门 (本身不授权)");
    CHECK(a.boot_disarm_seen, "启动门已打开");

    CHECK(!lc_arm_update(&a, arm_frame(3, true, false, false)), "只有 ENABLE 无 ARM => 不动作");
    CHECK(!lc_arm_update(&a, arm_frame(3, false, true, false)), "只有 ARM 无 ENABLE => 不动作");
    CHECK(lc_arm_update(&a, arm_frame(3, true, true, false)), "ENABLE+ARM+新令牌 => 授权");
    CHECK(lc_arm_armed(&a), "已授权");
    CHECK(lc_arm_token(&a) == 3u, "记住令牌");

    /* 保持授权: 同令牌后续帧 */
    CHECK(lc_arm_update(&a, arm_frame(3, true, true, false)), "同令牌保持授权");
    /* 主机失能: 撤销 */
    CHECK(!lc_arm_update(&a, arm_frame(3, false, false, false)), "收到无 ENABLE 帧 => 撤销");
    /* 旧令牌重放: 不能恢复 (停止后执行旧启动的防线) */
    CHECK(!lc_arm_update(&a, arm_frame(3, true, true, false)), "旧令牌重放被拒");
    CHECK(a.arm_reject >= 2u, "拒绝计数 (含启动门拒绝)");
    /* 新令牌: 新的明确授权 */
    CHECK(lc_arm_update(&a, arm_frame(4, true, true, false)), "新令牌重新授权");

    /* 急停: 立即撤销; 解除急停 (同一帧仍带 ENABLE+ARM+旧令牌) 不能恢复 */
    CHECK(!lc_arm_update(&a, arm_frame(4, true, true, true)), "急停帧 => 未授权");
    CHECK(!lc_arm_update(&a, arm_frame(4, true, true, false)), "解除急停后旧令牌不能自动恢复");
    CHECK(!lc_arm_armed(&a), "仍处于未授权");
    CHECK(lc_arm_update(&a, arm_frame(5, true, true, false)), "用户重新按使能(新令牌)才恢复");

    /* 断联恢复: force_safe 后, 断联前积压的授权帧重放必须被拒 */
    uint16_t old_frame = arm_frame(5, true, true, false);
    lc_arm_force_safe(&a);
    CHECK(!lc_arm_armed(&a), "断联 => 撤销授权");
    CHECK(!lc_arm_update(&a, old_frame), "断联前旧授权帧不能恢复运动");
    CHECK(!lc_arm_update(&a, old_frame), "同一旧帧重复到达仍被拒");
    CHECK(lc_arm_update(&a, arm_frame(6, true, true, false)), "新令牌才能恢复");

    /* 故障/CAN 恢复: 与断联同一语义 */
    lc_arm_force_safe(&a);
    CHECK(!lc_arm_update(&a, arm_frame(6, true, true, false)), "故障恢复后旧令牌不能恢复");
    CHECK(lc_arm_update(&a, arm_frame(7, true, true, false)), "新令牌恢复");

    /* 主机侧令牌生成: 每次明确使能都换令牌, 复位后也不重复 */
    lc_arm_tx_t tx;
    lc_arm_tx_init(&tx, 200u);
    uint8_t t1 = lc_arm_tx_next(&tx);
    uint8_t t2 = lc_arm_tx_next(&tx);
    CHECK(t1 != 0u && t2 != 0u, "令牌非零");
    CHECK(t1 != t2, "每次使能令牌必变");
    lc_arm_tx_clear(&tx);
    CHECK(!lc_arm_tx_active(&tx), "失能后主机侧授权失效");
    uint8_t t3 = lc_arm_tx_next(&tx);
    CHECK(t3 != t2 && t3 != 0u, "再次使能又是新令牌");

    /* 令牌 255 -> 1 回绕, 不会产生 0 */
    lc_arm_tx_t wrap;
    lc_arm_tx_init(&wrap, 254u);
    CHECK(lc_arm_tx_next(&wrap) == 255u, "255");
    CHECK(lc_arm_tx_next(&wrap) == 1u, "回绕跳过 0");
    CHECK(lc_arm_tx_next(&wrap) == 2u, "继续递增");
}

/* ================================================================== */
/* 5. 新鲜度                                                           */
/* ================================================================== */

static void test_freshness(void)
{
    printf("\n== 5. 反馈新鲜度 ==\n");

    lc_fresh_t f;
    lc_fresh_init(&f);
    bool have = true;
    CHECK(!lc_fresh_ok(&f, 1000u, 300u), "从未更新 => 不新鲜");
    CHECK(lc_fresh_age(&f, 1000u, &have) == 0xFFFFFFFFu && !have, "从未更新 => 标记 have=false");

    lc_fresh_kick(&f, 1000u);
    CHECK(lc_fresh_ok(&f, 1100u, 300u), "100ms 内新鲜");
    CHECK(lc_fresh_ok(&f, 1300u, 300u), "正好 300ms 仍算新鲜 (<=)");
    CHECK(!lc_fresh_ok(&f, 1301u, 300u), "超过 300ms => 过期");
    CHECK(lc_fresh_age(&f, 1400u, &have) == 400u && have, "年龄计算");

    /* 32 位毫秒回绕 */
    lc_fresh_init(&f);
    lc_fresh_kick(&f, 0xFFFFFF00u);
    CHECK(lc_fresh_ok(&f, 0x00000010u, 300u), "回绕后仍能判断新鲜");
    CHECK(!lc_fresh_ok(&f, 0x00000200u, 300u), "回绕后过期判断正确");
}

/* ================================================================== */
/* 6. ESP-NOW 发送统计                                                 */
/* ================================================================== */

static void test_txstats(void)
{
    printf("\n== 6. ESP-NOW 提交/回调/确认统计 ==\n");

    lc_txstats_t s;
    lc_txstats_init(&s);

    /* 控制帧提交成功, 回调还没来; 此时统计不能把结果算到本帧 */
    CHECK(lc_txstats_submit(&s, LC_TX_CTRL, 1u, 1000u, true), "提交控制帧");
    CHECK(s.submit_ok[LC_TX_CTRL] == 1u, "提交成功计数");
    CHECK(s.cb_ok[LC_TX_CTRL] == 0u && s.cb_fail[LC_TX_CTRL] == 0u, "回调未到 => 回调计数为 0");

    /* 中间插一帧增益: 回调按顺序分别记到各自类型 */
    CHECK(lc_txstats_submit(&s, LC_TX_PARAM, 2u, 1010u, true), "提交增益帧");
    lc_txstats_cb(&s, true, 1020u);    /* 控制帧回调 */
    lc_txstats_cb(&s, false, 1030u);   /* 增益帧回调 */
    CHECK(s.cb_ok[LC_TX_CTRL] == 1u, "控制帧回调成功");
    CHECK(s.cb_fail[LC_TX_PARAM] == 1u, "增益帧回调失败, 不串到控制帧");
    CHECK(s.cb_fail[LC_TX_CTRL] == 0u, "控制帧没有被误记失败");

    /* 提交失败: 不计入在途, 也不会有回调 */
    CHECK(!lc_txstats_submit(&s, LC_TX_CTRL, 3u, 1040u, false), "提交失败");
    CHECK(s.submit_fail[LC_TX_CTRL] == 1u, "提交失败计数");
    CHECK(lc_txstats_inflight(&s) == 0u, "提交失败不产生在途");

    /* 在途超时: 记录超时, 迟到回调不能算到下一帧 */
    CHECK(lc_txstats_submit(&s, LC_TX_CTRL, 4u, 2000u, true), "提交第二帧");
    CHECK(lc_txstats_expire(&s, 2300u, 250u) == 1u, "250ms 未回调 => 超时");
    CHECK(s.cb_timeout == 1u, "超时计数");
    CHECK(lc_txstats_inflight(&s) == 0u, "超时后在途清空");
    lc_txstats_cb(&s, true, 2310u);    /* 迟到回调 */
    CHECK(s.cb_unexpected == 1u, "迟到回调记为 unexpected, 不污染后续帧");

    /* 应用层确认单独计数 */
    lc_txstats_confirm(&s, LC_TX_CTRL);
    CHECK(s.confirm[LC_TX_CTRL] == 1u, "应用层确认计数独立");
}

/* ================================================================== */
/* 7. CAN 发送协调                                                     */
/* ================================================================== */

static lc_can_frame_t can_frame(lc_can_prio_t prio, uint8_t tag, uint32_t id)
{
    lc_can_frame_t f;
    memset(&f, 0, sizeof(f));
    f.id = id;
    f.len = 8;
    f.prio = (uint8_t)prio;
    f.tag = tag;
    for (uint8_t i = 0; i < 8; i++) {
        f.data[i] = (uint8_t)(id + i);
    }
    return f;
}

static void test_can_tx(void)
{
    printf("\n== 7. CAN 发送协调 ==\n");

    lc_can_tx_t t;
    lc_can_tx_init(&t);

    lc_can_frame_t mit = can_frame(LC_CAN_PRIO_MOTION, 0, 0x01);
    lc_can_frame_t mit2 = can_frame(LC_CAN_PRIO_MOTION, 0, 0x02);
    lc_can_frame_t disable = can_frame(LC_CAN_PRIO_SAFETY, LC_CAN_SAFETY_DISABLE, 0x03);
    lc_can_frame_t enable = can_frame(LC_CAN_PRIO_SAFETY, LC_CAN_SAFETY_ENABLE, 0x04);
    lc_can_frame_t param = can_frame(LC_CAN_PRIO_PARAM, 0, 0x7FF);

    /* 邮箱全满 (written=false): 安全命令不能丢, 必须留在队列里 */
    CHECK(lc_can_tx_request(&t, &disable), "提交失能命令");
    const lc_can_frame_t *p = lc_can_tx_peek(&t);
    CHECK(p != NULL && p->tag == LC_CAN_SAFETY_DISABLE, "失能命令优先");
    lc_can_tx_pop(&t, false);
    CHECK(lc_can_tx_pending(&t), "邮箱满时失能命令仍在队列 (未丢)");
    CHECK(t.submit_fail == 1u, "提交失败计数");
    CHECK(t.submitted == 0u, "没有写入邮箱就不算已提交");

    /* 邮箱空出来: 失能命令终于写入 */
    p = lc_can_tx_peek(&t);
    lc_can_tx_pop(&t, true);
    CHECK(t.submitted == 1u, "失能命令已提交");
    CHECK(!lc_can_tx_pending(&t), "失能命令发送后清空");

    /* 先请求使能再请求失能: 失能压过使能, 使能作废 */
    CHECK(lc_can_tx_request(&t, &enable), "提交使能");
    CHECK(lc_can_tx_request(&t, &disable), "提交失能");
    p = lc_can_tx_peek(&t);
    CHECK(p != NULL && p->tag == LC_CAN_SAFETY_DISABLE, "失能优先于使能");
    lc_can_tx_pop(&t, true);
    p = lc_can_tx_peek(&t);
    CHECK(p == NULL, "失能提交后, 排队的旧使能已经作废");

    /* 运动帧只保留最新: 队列里不会积压旧目标 */
    CHECK(lc_can_tx_request(&t, &mit), "提交运动帧 1");
    CHECK(lc_can_tx_request(&t, &mit2), "提交运动帧 2");
    CHECK(t.motion_coalesced == 1u, "旧运动帧被新目标覆盖");
    p = lc_can_tx_peek(&t);
    CHECK(p != NULL && p->id == 0x02u, "只保留最新运动帧");

    /* 停止屏障: 失能请求清掉所有排队运动帧 */
    CHECK(lc_can_tx_request(&t, &disable), "再次提交失能");
    CHECK(!t.have_motion, "请求失能时运动帧立刻作废");

    /* CAN 恢复: 丢弃运动/参数, 保留安全命令 */
    lc_can_tx_abort(&t);
    CHECK(t.recoveries == 1u, "恢复计数");
    CHECK(lc_can_tx_peek(&t) != NULL, "恢复后安全命令仍在队列, 会优先补发");

    /* 参数队列: 低优先级, 满了明确丢 */
    lc_can_tx_init(&t);
    CHECK(lc_can_tx_request(&t, &param), "参数 1");
    CHECK(lc_can_tx_request(&t, &param), "参数 2");
    CHECK(!lc_can_tx_request(&t, &param), "参数队列满 => 明确丢弃");
    CHECK(t.param_dropped == 1u, "参数丢弃计数");

    /* 运动帧永远不会因为参数帧而饥饿 */
    CHECK(lc_can_tx_request(&t, &mit), "提交运动帧");
    p = lc_can_tx_peek(&t);
    CHECK(p != NULL && p->prio == (uint8_t)LC_CAN_PRIO_MOTION, "运动优先于参数");

    /* 总线完成统计: 提交 != 总线成功 */
    lc_can_tx_report_bus(&t, true);
    lc_can_tx_report_bus(&t, false);
    CHECK(t.bus_ok == 1u && t.bus_fail == 1u, "总线完成/失败分开统计");
}

/* ================================================================== */
/* 8. 字节流成帧                                                       */
/* ================================================================== */

static void feed_frame(lc_rx_t *rx, const uint8_t *f, uint32_t now,
                       lc_rx_kind_t *last, uint8_t out[17], int *count)
{
    for (uint8_t i = 0; i < 17u; i++) {
        lc_rx_kind_t k = lc_rx_feed(rx, f[i], now, out);
        if (k != LC_RX_NONE && count != NULL) {
            (*count)++;
            *last = k;
        }
    }
}

static void test_frame_rx(void)
{
    printf("\n== 8. 字节流成帧 / 重新同步 ==\n");

    lc_rx_t rx;
    lc_rx_init(&rx);

    uint8_t ctrl[CTRL_FRAME_SIZE];
    ctrl_target_t tgt = { 1.0f, 2.0f, 3.0f, 4.0f };
    proto_encode_ctrl(ctrl, &tgt, CTRL_FLAG_ENABLE, MOTOR_MODE_MIT, 5);

    uint8_t out[17];
    lc_rx_kind_t last = LC_RX_NONE;
    int n = 0;
    feed_frame(&rx, ctrl, 1000u, &last, out, &n);
    CHECK(n == 1 && last == LC_RX_CTRL, "控制帧成帧");
    CHECK(memcmp(out, ctrl, 17) == 0, "成帧内容逐字节一致");

    /* 各种类型都能识别 */
    uint8_t st[STATUS_FRAME_SIZE];
    proto_encode_status(st, 1, 2, LINK_STATE_OK, 1.0f, 2.0f, 3, 0);
    n = 0;
    feed_frame(&rx, st, 1010u, &last, out, &n);
    CHECK(n == 1 && last == LC_RX_STATUS, "状态帧成帧");

    uint8_t g[GAINS_FRAME_SIZE];
    proto_encode_gains(g, 40.0f, 1.2f, 9);
    n = 0;
    feed_frame(&rx, g, 1020u, &last, out, &n);
    CHECK(n == 1 && last == LC_RX_GAINS, "增益帧成帧");

    uint8_t m[CTRL_FRAME_SIZE];
    proto_encode_motor_ext2(m, 1.0f, 2.0f, 3.0f, 0, 0, 0, 0, 1);
    n = 0;
    feed_frame(&rx, m, 1030u, &last, out, &n);
    CHECK(n == 1 && last == LC_RX_MOTOR, "电机帧成帧");

    uint8_t bs[BRIDGE_STATUS_FRAME_SIZE];
    proto_encode_bridge_status(bs, 1, PROTO_BR_FLAG_STM_FRESH, 5, 1, 0, 1, 0, 0, 1, 0, 0, 0);
    n = 0;
    feed_frame(&rx, bs, 1040u, &last, out, &n);
    CHECK(n == 1 && last == LC_RX_BRIDGE_STATUS, "桥接新鲜度帧成帧");

    /* 坏 CRC 帧后面紧跟一个好帧: 不能把好帧一起丢掉 */
    uint8_t bad[CTRL_FRAME_SIZE];
    memcpy(bad, ctrl, sizeof(bad));
    bad[9] ^= 0xFFu;   /* 破坏载荷 => CRC 失败 */
    n = 0;
    feed_frame(&rx, bad, 1050u, &last, out, &n);
    feed_frame(&rx, ctrl, 1060u, &last, out, &n);
    CHECK(n == 1 && last == LC_RX_CTRL, "坏帧之后的好帧仍能成帧");
    CHECK(rx.frames_bad >= 1u, "坏帧计数");
    CHECK(rx.crc_bad >= 1u, "CRC 错误计数");

    /* 纯噪声后重同步 */
    uint32_t before = rx.frames_ok;
    for (uint8_t i = 0; i < 40u; i++) {
        (void)lc_rx_feed(&rx, 0x00u, 1070u, out);
    }
    feed_frame(&rx, ctrl, 1080u, &last, out, &n);
    CHECK(rx.frames_ok == before + 1u, "噪声后能重新同步");

    /* DMA 溢出: 丢弃半帧后仍能重新同步 */
    lc_rx_overflow(&rx);
    feed_frame(&rx, ctrl, 1090u, &last, out, &n);
    CHECK(rx.frames_ok >= before + 2u, "溢出后能恢复成帧");
    CHECK(rx.overflow == 1u, "溢出计数");
    CHECK(rx.have_valid && rx.last_valid_ms == 1090u, "最后有效帧时间戳");
}

/* ================================================================== */
/* 9. 组合场景: 断联 -> 重连 -> 旧启动帧 -> 新启动                     */
/* ================================================================== */

static void test_reconnect_scenario(void)
{
    printf("\n== 9. 组合场景: 断联重连不得自动恢复运动 ==\n");

    /* 断联前: 主机授权令牌 T=11, 桥接/STM32 已 armed */
    lc_arm_t arm;
    lc_arm_init(&arm);
    uint16_t f_old = proto_flags_set_token(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 11u);
    CHECK(!lc_arm_update(&arm, f_old), "复位后启动门未开 => 旧授权帧被拒");
    CHECK(!lc_arm_update(&arm, arm_frame(0, false, false, false)), "先收到失能帧, 打开启动门");
    CHECK(lc_arm_update(&arm, f_old), "断联前已授权");

    /* 断联: STM32 侧撤销; 桥接队列里还压着断联前的 3 条 ENABLE 帧 */
    lc_arm_force_safe(&arm);
    lc_cmdq_t q;
    lc_cmdq_init(&q);
    for (uint8_t i = 0; i < 3; i++) {
        lc_cmd_t c;
        memset(&c, 0, sizeof(c));
        c.flags = f_old;
        c.seq = (uint8_t)(30u + i);
        c.rx_ms = 1000u;
        (void)lc_cmdq_push(&q, &c);
    }
    /* 恢复后: 这些帧要么过期要么被拒, 绝不能重新 armed */
    uint32_t purged = lc_cmdq_purge_motion(&q);
    CHECK(purged == 3u, "恢复时清掉断联前积压的启动帧");
    lc_arm_force_safe(&arm);
    CHECK(!lc_arm_update(&arm, f_old), "即使旧帧漏过来也不能授权");

    /* 用户重新按使能: 新令牌 => 允许 */
    uint16_t f_new = proto_flags_set_token(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 12u);
    CHECK(lc_arm_update(&arm, f_new), "用户明确重新使能后才允许运动");
    CHECK(lc_arm_armed(&arm), "已授权");
}


static void test_review_regressions(void)
{
    lc_txstats_t tx;
    lc_txstats_init(&tx);
    CHECK(lc_txstats_submit(&tx, LC_TX_CTRL, 1, 0, true), "A submission");
    CHECK(lc_txstats_expire(&tx, 251, 250) == 1, "A callback timed out");
    CHECK(tx.cb_desynced, "missing callback invalidates attribution");
    CHECK(tx.cb_fail[LC_TX_CTRL] == 0, "timeout is unknown, not failed delivery");
    CHECK(lc_txstats_submit(&tx, LC_TX_PARAM, 2, 252, true), "B may still be sent");
    lc_txstats_cb(&tx, false, 253); /* A arrives after B was submitted. */
    CHECK(tx.cb_fail[LC_TX_PARAM] == 0, "late A never becomes B failure");
    lc_txstats_cb(&tx, true, 254);
    CHECK(tx.cb_ok[LC_TX_PARAM] == 0, "uncertain callback is not invented success");
    CHECK(tx.cb_unexpected == 2, "uncorrelated callbacks counted explicitly");
    CHECK(lc_txstats_inflight(&tx) == 0, "unknown attribution does not block transport");

    lc_can_tx_t can;
    lc_can_tx_init(&can);
    lc_can_frame_t dis = {0}, en = {0};
    dis.prio = LC_CAN_PRIO_SAFETY; dis.tag = LC_CAN_SAFETY_DISABLE;
    en.prio = LC_CAN_PRIO_SAFETY; en.tag = LC_CAN_SAFETY_ENABLE;
    lc_can_tx_request(&can, &en);
    lc_can_tx_request(&can, &dis);
    CHECK(!can.safety_pending[LC_CAN_SAFETY_ENABLE], "stop removes older enable");
    lc_can_tx_request(&can, &en); /* Explicit new enable after the stop barrier. */
    CHECK(lc_can_tx_peek(&can)->tag == LC_CAN_SAFETY_DISABLE, "disable still sent first");
    lc_can_tx_pop(&can, true);
    CHECK(can.safety_pending[LC_CAN_SAFETY_ENABLE], "new explicit enable is preserved");

    CHECK(lc_stop_command("V 0") == LC_STOP_YAW, "yaw stop bypasses full queue");
    CHECK(lc_stop_command("U -0.0\r\n") == LC_STOP_PITCH, "pitch zero stop recognized");
    CHECK(lc_stop_command(" W 0 ") == LC_STOP_CRUISE, "cruise stop recognized");
    CHECK(lc_stop_command("x") == LC_STOP_ESTOP, "emergency stop");
    CHECK(lc_stop_command("C") == LC_STOP_DISABLE, "disable");
    CHECK(lc_stop_command("R") == LC_STOP_RELEASE, "release");
    CHECK(lc_stop_command("V 1") == LC_STOP_NONE, "nonzero movement stays normal");
    CHECK(lc_stop_command("V") == LC_STOP_NONE, "missing argument not a stop");
    CHECK(lc_stop_command("V 0bad") == LC_STOP_NONE, "invalid argument rejected");
    CHECK(lc_stop_command("R1") == LC_STOP_NONE, "invalid release rejected");
    CHECK(lc_stop_command("") == LC_STOP_NONE, "empty command");
    CHECK(lc_stop_command(NULL) == LC_STOP_NONE, "null command");
}

int main(void)
{
    test_review_regressions();
    printf("========================================\n");
    printf("  靶车 链路可靠性内核 自测\n");
    printf("========================================\n");

    test_protocol_v11();
    test_seq_window();
    test_cmd_queue();
    test_arm_guard();
    test_freshness();
    test_txstats();
    test_can_tx();
    test_frame_rx();
    test_reconnect_scenario();

    printf("\n========================================\n");
    if (g_fail == 0) {
        printf("  全部通过\n");
    } else {
        printf("  失败 %d 项\n", g_fail);
    }
    printf("========================================\n");
    return g_fail == 0 ? 0 : 1;
}
