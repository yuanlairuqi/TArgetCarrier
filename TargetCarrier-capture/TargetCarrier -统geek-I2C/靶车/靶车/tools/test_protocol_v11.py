#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
靶车协议 v1.1 · Python 独立实现对拍 / 行为模型测试
====================================================================

设计目的
--------
本文件**不 import** `tools/pc_monitor.py`，而是用标准库从零重写一遍 v1.1 新增
内容的编解码逻辑，和大端/小端、位域顺序、CRC 覆盖范围逐字节互相印证：

  * CRC16-Modbus
  * 控制帧（17 B，含 ARM 位 bit4 与 8 bit ARM_TOKEN）
  * CTRL_FLAG_FAILSAFE（bit5，桥接本地注入的安全帧）
  * 桥接 → 主机 新鲜度帧 `PROTO_MSG_BRIDGE_STATUS = 0x07`（字段见契约 §1.2）
  * 电机扩展帧 p[12..14]（安全位 / 丢弃计数 / ARM 回显）
  * 8 bit 序号窗口（重复 / 乱序 / 回绕 / 窗口边界）
  * 命令过期模型（100 ms 上限、32 位毫秒回绕、安全命令优先、停止屏障）

为什么值得单独跑
----------------
协议是"主机说通过了、板子上不通"的高发区。C 端有 test_protocol.c /
test_linkcore.c；这里是第三种语言、第三份实现。任何一处字节偏移或位定义
被改错，必然有一方对不上。

用法
----
    python tools/test_protocol_v11.py
    python tools/test_protocol_v11.py -v        # 每条断言都打印

退出码：全部通过 0；有失败 1；脚本自身异常 2。

依赖：仅 Python 3.8+ 标准库（不需要 pyserial，不连硬件）。
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# ============================================================================
# 一、协议常量 —— 必须与 firmware/lib/protocol/protocol.h / 契约 v1.1 一致
# ============================================================================

PROTO_HEAD = 0xAA
PROTO_TAIL = 0x55
PROTO_STX = 0x5A

PROTO_MSG_CTRL = 0x01
PROTO_MSG_STATUS = 0x02
PROTO_MSG_GAINS = 0x03
PROTO_MSG_PARAM = 0x04
PROTO_MSG_PARAM_REPLY = 0x05
PROTO_MSG_MOTOR = 0x06
PROTO_MSG_BRIDGE_STATUS = 0x07

CTRL_FRAME_SIZE = 17
STATUS_FRAME_SIZE = 17
BRIDGE_STATUS_FRAME_SIZE = 17

ANGLE_SCALE = 0.01
RATE_SCALE = 0.1
YAW_LIMIT = 180.0
PITCH_LIMIT = 90.0
RATE_LIMIT = 180.0

# 控制 flags（protocol.h §v1.1）
CTRL_FLAG_ENABLE = 0x0001
CTRL_FLAG_CLEAR_ERR = 0x0002
CTRL_FLAG_SET_ZERO = 0x0004
CTRL_FLAG_ESTOP = 0x0008
CTRL_FLAG_ARM = 0x0010
CTRL_FLAG_FAILSAFE = 0x0020          # bit5：桥接本地注入，非主机原帧
CTRL_FLAG_ARM_TOKEN_MASK = 0xFF00
CTRL_FLAG_ARM_TOKEN_SHIFT = 8
CTRL_ARM_TOKEN_NONE = 0

# 桥接安全判定：这些类型的帧必须走 safety 队列（契约 §5.2）
CTRL_SAFETY_BITS = CTRL_FLAG_ESTOP | CTRL_FLAG_FAILSAFE | CTRL_FLAG_CLEAR_ERR | CTRL_FLAG_SET_ZERO

# 桥接 → 主机 新鲜度帧 flags（契约 §1.2）
PROTO_BR_FLAG_STM_FRESH = 0x01
PROTO_BR_FLAG_FAILSAFE = 0x02
PROTO_BR_FLAG_HAVE_STM = 0x04
PROTO_BR_FLAG_MOTION_BLOCKED = 0x08
PROTO_BR_FLAG_UPLINK_READY = 0x10
PROTO_BR_FLAG_STM_ESTOP = 0x20
PROTO_BR_FLAG_RX_OVERFLOW = 0x40

# 电机扩展帧 p[12] 安全位（protocol.h v1.1）
PROTO_MOTOR_SAFETY_ARMED = 0x01
PROTO_MOTOR_SAFETY_ENABLED = 0x02
PROTO_MOTOR_SAFETY_ESTOP = 0x04
PROTO_MOTOR_SAFETY_FAULT = 0x08
PROTO_MOTOR_SAFETY_FBK_STALE = 0x10
PROTO_MOTOR_SAFETY_CAN_RECOVER = 0x20
PROTO_MOTOR_SAFETY_LINK_LOST = 0x40
PROTO_MOTOR_SAFETY_ARM_REJECT = 0x80

MOTOR_POS_SCALE = 0.01
MOTOR_VEL_SCALE = 0.01
MOTOR_TAU_SCALE = 0.001

MOTOR_MODE_MIT = 0
MOTOR_MODE_POS = 1
MOTOR_MODE_SPD = 2
MOTOR_MODE_PSI = 3

LINK_STATE_INIT = 0
LINK_STATE_OK = 1
LINK_STATE_TIMEOUT = 2
LINK_STATE_ESTOP = 3
LINK_STATE_FAULT = 4

# 三端保护时限（契约 §2，文档/代码/测试必须同一口径）
CTRL_CMD_MAX_AGE_MS = 100
BRIDGE_MASTER_TIMEOUT_MS = 300
BRIDGE_FAILSAFE_PERIOD_MS = 50
BRIDGE_STM_FRESH_MS = 300
BRIDGE_UPLINK_PERIOD_MS = 100
STM_CTRL_TIMEOUT_MS = 300
STM_CMD_MAX_AGE_MS = 100
STM_FB_TIMEOUT_MS = 200
STM_FB_GRACE_MS = 1000
MASTER_STM_STALE_MS = 300
MASTER_BRIDGE_STALE_MS = 300
ESPNOW_CB_TIMEOUT_MS = 250
WS_CMD_Q_LEN = 8
CAN_RECOVER_THRESHOLD = 64
CAN_RECOVER_PERIOD_MS = 2000


# ============================================================================
# 二、独立实现的编解码
# ============================================================================

def crc16_modbus(data: bytes) -> int:
    """CRC16-Modbus，与 protocol.h 的 proto_crc16 逐位一致（多项式 0xA001，初值 0xFFFF）"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


def _clamp(v: float, lo: float, hi: float) -> float:
    return lo if v < lo else (hi if v > hi else v)


def _round_i16(v: float) -> int:
    return int(v + 0.5) if v >= 0 else int(v - 0.5)


def _s16(v: int) -> int:
    return v - 0x10000 if v & 0x8000 else v


def deg_to_q(deg: float, limit: float) -> int:
    return _round_i16(_clamp(deg, -limit, limit) / ANGLE_SCALE)


def dps_to_q(dps: float) -> int:
    return _round_i16(_clamp(dps, -RATE_LIMIT, RATE_LIMIT) / RATE_SCALE)


# ---------------------------------------------------------------- 控制帧

def encode_ctrl(seq: int, flags: int, mode: int,
                yaw_deg: float, pitch_deg: float,
                yaw_rate_dps: float = 0.0, pitch_rate_dps: float = 0.0) -> bytes:
    """17 字节控制帧。布局与 proto_encode_ctrl 完全一致。"""
    yaw_q = deg_to_q(yaw_deg, YAW_LIMIT)
    pitch_q = deg_to_q(pitch_deg, PITCH_LIMIT)
    yr_q = dps_to_q(yaw_rate_dps)
    pr_q = dps_to_q(pitch_rate_dps)

    f = bytearray(CTRL_FRAME_SIZE)
    f[0] = PROTO_HEAD
    f[1] = 13
    f[2] = PROTO_MSG_CTRL
    f[3] = seq & 0xFF
    f[4] = flags & 0xFF
    f[5] = (flags >> 8) & 0xFF
    f[6] = mode & 0xFF
    f[7] = yaw_q & 0xFF
    f[8] = (yaw_q >> 8) & 0xFF
    f[9] = pitch_q & 0xFF
    f[10] = (pitch_q >> 8) & 0xFF
    f[11] = yr_q & 0xFF
    f[12] = (yr_q >> 8) & 0xFF
    f[13] = pr_q & 0xFF
    f[14] = (pr_q >> 8) & 0xFF
    f[15] = crc16_modbus(bytes(f[1:15])) & 0xFF
    f[16] = PROTO_TAIL
    return bytes(f)


def decode_ctrl(frame: bytes):
    """校验并解析控制帧；任何一处不对返回 None（模拟旧固件"解码失败即忽略"）。"""
    if len(frame) != CTRL_FRAME_SIZE:
        return None
    if frame[0] != PROTO_HEAD or frame[1] != 13 or frame[2] != PROTO_MSG_CTRL:
        return None
    if frame[16] != PROTO_TAIL:
        return None
    if (crc16_modbus(frame[1:15]) & 0xFF) != frame[15]:
        return None

    flags = frame[4] | (frame[5] << 8)
    return {
        "seq": frame[3],
        "flags": flags,
        "enable": bool(flags & CTRL_FLAG_ENABLE),
        "clear_err": bool(flags & CTRL_FLAG_CLEAR_ERR),
        "set_zero": bool(flags & CTRL_FLAG_SET_ZERO),
        "estop": bool(flags & CTRL_FLAG_ESTOP),
        "arm": bool(flags & CTRL_FLAG_ARM),
        "failsafe": bool(flags & CTRL_FLAG_FAILSAFE),
        "token": proto_flags_token(flags),
        "mode": frame[6],
        "yaw_q": _s16(frame[7] | (frame[8] << 8)),
        "pitch_q": _s16(frame[9] | (frame[10] << 8)),
        "yawrate_q": _s16(frame[11] | (frame[12] << 8)),
        "pitchrate_q": _s16(frame[13] | (frame[14] << 8)),
        "yaw_deg": _s16(frame[7] | (frame[8] << 8)) * ANGLE_SCALE,
        "pitch_deg": _s16(frame[9] | (frame[10] << 8)) * ANGLE_SCALE,
        "yaw_rate_dps": _s16(frame[11] | (frame[12] << 8)) * RATE_SCALE,
        "pitch_rate_dps": _s16(frame[13] | (frame[14] << 8)) * RATE_SCALE,
    }


# ---------------------------------------------------------------- flags / 令牌

def proto_flags_token(flags: int) -> int:
    return (flags & CTRL_FLAG_ARM_TOKEN_MASK) >> CTRL_FLAG_ARM_TOKEN_SHIFT


def proto_flags_set_token(flags: int, token: int) -> int:
    flags &= ~CTRL_FLAG_ARM_TOKEN_MASK & 0xFFFF
    return (flags | ((token & 0xFF) << CTRL_FLAG_ARM_TOKEN_SHIFT)) & 0xFFFF


def proto_frame_requests_arm(flags: int, last_accepted_token: int) -> bool:
    """与 protocol.h 的 proto_frame_requests_arm 同语义（只判断帧本身授权语义）"""
    if not (flags & CTRL_FLAG_ENABLE):
        return False
    if not (flags & CTRL_FLAG_ARM):
        return False
    token = proto_flags_token(flags)
    if token == CTRL_ARM_TOKEN_NONE:
        return False
    return token != last_accepted_token


class ArmState:
    """STM32 侧 lc_arm_update/lc_arm_force_safe 的独立模型。

    与 lc_guard.c 的语义保持一致：
      - ESTOP 优先级最高，无条件撤销；
      - 缺 ENABLE 或缺 ARM 也撤销；
      - 已 arm 时同令牌帧是保持，换令牌视为新授权；
      - 未 arm 时只有"非上次已接受令牌"才能重新 arm（防重放/防断联自动恢复）；
      - force_safe 不清 last_token，所以旧令牌永远不能再次授权。
    """

    def __init__(self) -> None:
        self.last_token = CTRL_ARM_TOKEN_NONE
        self.armed = False
        self.arm_ok = 0
        self.arm_reject = 0
        self.disarm = 0
        self.safe_events = 0

    def update(self, flags: int) -> bool:
        if flags & CTRL_FLAG_ESTOP:
            if self.armed:
                self.disarm += 1
            self.armed = False
            return False

        arm_bit = bool(flags & CTRL_FLAG_ARM)
        enable_bit = bool(flags & CTRL_FLAG_ENABLE)
        token = proto_flags_token(flags)

        if not arm_bit or not enable_bit:
            if self.armed:
                self.disarm += 1
            self.armed = False
            return False

        if token == CTRL_ARM_TOKEN_NONE:
            self.arm_reject += 1
            return False

        if self.armed:
            if token != self.last_token:
                self.last_token = token
                self.arm_ok += 1
            return True

        if token == self.last_token:
            self.arm_reject += 1
            return False

        self.last_token = token
        self.armed = True
        self.arm_ok += 1
        return True

    def force_safe(self) -> None:
        # 注意：不清 last_token —— 旧令牌必须永远失效
        if self.armed:
            self.disarm += 1
        self.armed = False
        self.safe_events += 1


# ---------------------------------------------------------------- 桥接新鲜度帧

def encode_bridge_status(seq: int, flags: int, stm_age_10ms: int, stm_state: int,
                         stm_err: int, rx_ok_l8: int, rx_dup_l8: int, rx_drop_l8: int,
                         fwd_ok_l8: int, failsafe_l8: int, tx_fail_l8: int,
                         arm_reject_l8: int) -> bytes:
    """17 字节 0x07 帧，布局严格按契约 §1.2。"""
    f = bytearray(BRIDGE_STATUS_FRAME_SIZE)
    f[0] = PROTO_HEAD
    f[1] = 13
    f[2] = PROTO_MSG_BRIDGE_STATUS
    f[3] = seq & 0xFF
    f[4] = flags & 0xFF
    f[5] = stm_age_10ms & 0xFF
    f[6] = stm_state & 0xFF
    f[7] = stm_err & 0xFF
    f[8] = rx_ok_l8 & 0xFF
    f[9] = rx_dup_l8 & 0xFF
    f[10] = rx_drop_l8 & 0xFF
    f[11] = fwd_ok_l8 & 0xFF
    f[12] = failsafe_l8 & 0xFF
    f[13] = tx_fail_l8 & 0xFF
    f[14] = arm_reject_l8 & 0xFF
    f[15] = crc16_modbus(bytes(f[1:15])) & 0xFF
    f[16] = PROTO_TAIL
    return bytes(f)


def decode_bridge_status(frame: bytes):
    if len(frame) != BRIDGE_STATUS_FRAME_SIZE:
        return None
    if frame[0] != PROTO_HEAD or frame[1] != 13:
        return None
    if frame[2] != PROTO_MSG_BRIDGE_STATUS:
        return None
    if frame[16] != PROTO_TAIL:
        return None
    if (crc16_modbus(frame[1:15]) & 0xFF) != frame[15]:
        return None
    return {
        "seq": frame[3],
        "flags": frame[4],
        "stm_age_10ms": frame[5],
        "stm_age_ms": frame[5] * 10,
        "stm_state": frame[6],
        "stm_err": frame[7],
        "rx_ok_l8": frame[8],
        "rx_dup_l8": frame[9],
        "rx_drop_l8": frame[10],
        "fwd_ok_l8": frame[11],
        "failsafe_l8": frame[12],
        "tx_fail_l8": frame[13],
        "arm_reject_l8": frame[14],
    }


# ---------------------------------------------------------------- 电机扩展帧

def encode_motor_ext2(seq: int, pos: float, vel: float, tau: float, can_fail: int,
                      safety: int, cmd_drops: int, arm_echo: int) -> bytes:
    p = _clamp(pos / MOTOR_POS_SCALE, -32768.0, 32767.0)
    v = _clamp(vel / MOTOR_VEL_SCALE, -32768.0, 32767.0)
    t = _clamp(tau / MOTOR_TAU_SCALE, -32768.0, 32767.0)
    pq, vq, tq = int(p), int(v), int(t)

    f = bytearray(CTRL_FRAME_SIZE)
    f[0] = PROTO_HEAD
    f[1] = 13
    f[2] = PROTO_MSG_MOTOR
    f[3] = seq & 0xFF
    f[4] = pq & 0xFF
    f[5] = (pq >> 8) & 0xFF
    f[6] = vq & 0xFF
    f[7] = (vq >> 8) & 0xFF
    f[8] = tq & 0xFF
    f[9] = (tq >> 8) & 0xFF
    f[10] = can_fail & 0xFF
    f[11] = (can_fail >> 8) & 0xFF
    f[12] = safety & 0xFF
    f[13] = cmd_drops & 0xFF
    f[14] = arm_echo & 0xFF
    f[15] = crc16_modbus(bytes(f[1:15])) & 0xFF
    f[16] = PROTO_TAIL
    return bytes(f)


def decode_motor_ext2(frame: bytes):
    if len(frame) != CTRL_FRAME_SIZE:
        return None
    if frame[0] != PROTO_HEAD or frame[1] != 13 or frame[2] != PROTO_MSG_MOTOR:
        return None
    if frame[16] != PROTO_TAIL:
        return None
    if (crc16_modbus(frame[1:15]) & 0xFF) != frame[15]:
        return None
    return {
        "seq": frame[3],
        "pos": _s16(frame[4] | (frame[5] << 8)) * MOTOR_POS_SCALE,
        "vel": _s16(frame[6] | (frame[7] << 8)) * MOTOR_VEL_SCALE,
        "tau": _s16(frame[8] | (frame[9] << 8)) * MOTOR_TAU_SCALE,
        "can_fail": frame[10] | (frame[11] << 8),
        "safety": frame[12],
        "cmd_drops": frame[13],
        "arm_echo": frame[14],
    }


# ---------------------------------------------------------------- 序号窗口

def seq_cmp8(a: int, b: int) -> int:
    """RFC1982 风格：a 比 b 新返回 1，旧返回 -1，相同返回 0。"""
    d = (a - b) & 0xFF
    if d == 0:
        return 0
    return 1 if d < 0x80 else -1


class SeqGate:
    """lc_seq_accept 的独立模型：只接受比已接受最新序号更新的帧。"""

    def __init__(self) -> None:
        self.last = 0
        self.have = False
        self.accepted = 0
        self.duplicate = 0
        self.reorder = 0

    def accept(self, seq: int) -> bool:
        seq &= 0xFF
        if not self.have:
            self.have = True
            self.last = seq
            self.accepted += 1
            return True
        cmp = seq_cmp8(seq, self.last)
        if cmp == 0:
            self.duplicate += 1
            return False
        if cmp < 0:
            self.reorder += 1
            return False
        self.last = seq
        self.accepted += 1
        return True


def age_exceeds(now_ms: int, rx_ms: int, limit_ms: int) -> bool:
    """32 位毫秒回绕安全：只比较无符号差（对应 lc_age_exceeds）。"""
    elapsed = (now_ms - rx_ms) & 0xFFFFFFFF
    return elapsed > limit_ms


# ---------------------------------------------------------------- 命令过期模型

class CmdQueue:
    """STM32 命令队列的简化模型：安全命令优先、过期丢弃、序号过滤、停止屏障。

    这里不追求复刻 lc_cmdq 的全部实现，只验证安全不变量：
      - 过期命令绝不能被执行；
      - 安全命令排在运动命令之前；
      - 停止/急停会清掉排队中的运动命令，安全命令保留。
    """

    def __init__(self) -> None:
        self.items = []
        self.expired = 0
        self.purged_motion = 0

    def push(self, seq: int, rx_ms: int, safety: bool = False) -> None:
        self.items.append({"seq": seq, "rx_ms": rx_ms, "safety": safety})

    def purge_motion(self) -> int:
        n = sum(1 for it in self.items if not it["safety"])
        self.items = [it for it in self.items if it["safety"]]
        self.purged_motion += n
        return n

    def pop(self, now_ms: int, max_age_ms: int, gate: SeqGate):
        while self.items:
            idx = next((i for i, it in enumerate(self.items) if it["safety"]), 0)
            cmd = self.items.pop(idx)
            if max_age_ms and age_exceeds(now_ms, cmd["rx_ms"], max_age_ms):
                self.expired += 1
                continue
            if not gate.accept(cmd["seq"]):
                continue
            return cmd
        return None


# ============================================================================
# 三、测试框架
# ============================================================================

class Runner:
    def __init__(self, verbose: bool) -> None:
        self.verbose = verbose
        self.passed = 0
        self.failed = 0
        self._section = ""

    def section(self, title: str) -> None:
        self._section = title
        print("\n== " + title + " ==")

    def check(self, cond: bool, msg: str) -> bool:
        cond = bool(cond)
        if cond:
            self.passed += 1
            if self.verbose:
                print("  [PASS] " + msg)
        else:
            self.failed += 1
            print("  [FAIL] " + msg)
        return cond

    def eq(self, got, expect, msg: str) -> bool:
        ok = got == expect
        if not ok:
            return self.check(False, "%s（got=%r expect=%r）" % (msg, got, expect))
        return self.check(True, msg)

    def near(self, got: float, expect: float, tol: float, msg: str) -> bool:
        ok = abs(got - expect) <= tol
        if not ok:
            return self.check(False, "%s（got=%r expect=%r±%r）" % (msg, got, expect, tol))
        return self.check(True, msg)


# ============================================================================
# 四、测试用例
# ============================================================================

def test_crc(r: Runner) -> None:
    r.section("CRC16-Modbus（标准向量 / 初值 / 与帧内 CRC 覆盖范围）")
    r.eq(crc16_modbus(b"123456789"), 0x4B37, "CRC(\"123456789\") == 0x4B37")
    r.eq(crc16_modbus(b""), 0xFFFF, "空数据初值 == 0xFFFF")
    r.eq(crc16_modbus(bytes([0x01, 0x02, 0x03])), crc16_modbus(bytes([0x01, 0x02, 0x03])),
         "同样输入结果稳定（无隐藏状态）")

    frame = encode_ctrl(seq=1, flags=0, mode=MOTOR_MODE_MIT, yaw_deg=0, pitch_deg=0)
    r.eq(frame[15], crc16_modbus(frame[1:15]) & 0xFF,
         "控制帧 CRC = CRC16(bytes[1..14]) 低 8 位")
    r.eq(frame[16], PROTO_TAIL, "控制帧以 0x55 结尾")


def test_ctrl_frame(r: Runner) -> None:
    r.section("控制帧编解码（线格式逐字节 + 坏帧拒绝 + 限幅）")
    flags = CTRL_FLAG_ENABLE | CTRL_FLAG_CLEAR_ERR
    frame = encode_ctrl(seq=7, flags=flags, mode=MOTOR_MODE_MIT,
                        yaw_deg=12.34, pitch_deg=-45.67,
                        yaw_rate_dps=88.0, pitch_rate_dps=-12.5)

    r.eq(len(frame), CTRL_FRAME_SIZE, "帧长 17 B")
    r.eq(frame[0], PROTO_HEAD, "[0] 帧头 == 0xAA")
    r.eq(frame[1], 13, "[1] len == 13")
    r.eq(frame[2], PROTO_MSG_CTRL, "[2] type == 0x01")
    r.eq(frame[3], 7, "[3] seq == 7")
    r.eq(frame[4], 0x03, "[4] flags 低字节 == ENABLE|CLEAR_ERR")
    r.eq(frame[5], 0x00, "[5] flags 高字节 == 0")
    r.eq(frame[15], crc16_modbus(frame[1:15]) & 0xFF, "[15] CRC 低字节覆盖 [1..14]")
    r.eq(frame[16], PROTO_TAIL, "[16] 帧尾 == 0x55")

    dec = decode_ctrl(frame)
    r.check(dec is not None, "往返解析成功")
    if dec:
        r.eq(dec["seq"], 7, "往返 seq")
        r.eq(dec["flags"], flags, "往返 flags")
        r.check(dec["enable"] and dec["clear_err"], "往返 ENABLE/CLEAR_ERR 位")
        r.check(not dec["estop"] and not dec["arm"], "未置位的 ESTOP/ARM 不误报")
        r.eq(dec["mode"], MOTOR_MODE_MIT, "往返 mode")
        r.near(dec["yaw_deg"], 12.34, ANGLE_SCALE / 2, "往返 yaw（量化 0.01°）")
        r.near(dec["pitch_deg"], -45.67, ANGLE_SCALE / 2, "往返 pitch")
        r.near(dec["yaw_rate_dps"], 88.0, RATE_SCALE / 2, "往返 yaw rate（量化 0.1 dps）")
        r.near(dec["pitch_rate_dps"], -12.5, RATE_SCALE / 2, "往返 pitch rate")

    # 坏帧一律拒绝
    for name, mutate in [
        ("数据位翻转", lambda b: b.__setitem__(8, b[8] ^ 0x01)),
        ("帧头错", lambda b: b.__setitem__(0, 0xAB)),
        ("帧尾错", lambda b: b.__setitem__(16, 0x00)),
        ("len 错", lambda b: b.__setitem__(1, 12)),
        ("type 错", lambda b: b.__setitem__(2, 0x07)),
        ("CRC 错", lambda b: b.__setitem__(15, (b[15] + 1) & 0xFF)),
    ]:
        bad = bytearray(frame)
        mutate(bad)
        r.check(decode_ctrl(bytes(bad)) is None, "坏帧拒绝：" + name)
    r.check(decode_ctrl(frame[:-1]) is None, "截断帧（16 B）拒绝")
    r.check(decode_ctrl(frame + b"\x00") is None, "超长帧（18 B）拒绝")

    # 限幅（协议层必须自己夹，不能指望上层）
    big = decode_ctrl(encode_ctrl(0, 0, MOTOR_MODE_MIT, 999.0, -999.0, 999.0, -999.0))
    if big:
        r.eq(big["yaw_q"], 18000, "yaw 超限夹到 +180°")
        r.eq(big["pitch_q"], -9000, "pitch 超限夹到 -90°")
        r.eq(big["yawrate_q"], 1800, "yaw rate 超限夹到 +180 dps")
        r.eq(big["pitchrate_q"], -1800, "pitch rate 超限夹到 -180 dps")
    for mode in (MOTOR_MODE_POS, MOTOR_MODE_SPD, MOTOR_MODE_PSI):
        d = decode_ctrl(encode_ctrl(0, 0, mode, 0, 0))
        r.check(d is not None and d["mode"] == mode, "四类电机模式可编码 mode=%d" % mode)


def test_arm_token(r: Runner) -> None:
    r.section("ARM 位 + 8 bit 令牌（位定义 / 帧语义 / 授权状态机 / FAILSAFE）")
    base = CTRL_FLAG_ENABLE
    r.eq(CTRL_FLAG_ARM, 0x0010, "ARM 位 == bit4 (0x0010)")
    r.eq(CTRL_FLAG_FAILSAFE, 0x0020, "FAILSAFE 位 == bit5 (0x0020)")
    r.eq(CTRL_FLAG_ARM_TOKEN_MASK, 0xFF00, "令牌掩码 == bit15:8")
    r.eq(CTRL_FLAG_ARM & CTRL_FLAG_FAILSAFE, 0, "ARM 与 FAILSAFE 位不冲突")

    r.eq(proto_flags_token(base), 0, "未设令牌时 token == 0")
    f = proto_flags_set_token(base, 0xA5)
    r.eq(proto_flags_token(f), 0xA5, "set_token/token 往返 0xA5")
    r.eq(proto_flags_set_token(f, 0x00) & ~CTRL_FLAG_ARM_TOKEN_MASK & 0xFFFF, base,
         "set_token(0) 只清令牌，不动低位")
    r.eq(proto_flags_set_token(base, 0x1FF) >> 8, 0xFF, "令牌写入截断到 8 bit")

    tok5 = proto_flags_set_token(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 5)
    r.check(proto_frame_requests_arm(tok5, 0), "ENABLE+ARM+tok5 是合法首次授权")
    r.check(proto_frame_requests_arm(tok5, 4), "换新令牌视为新授权")
    r.check(not proto_frame_requests_arm(tok5, 5), "同一令牌重放不算新授权")
    r.check(not proto_frame_requests_arm(CTRL_FLAG_ARM | proto_flags_set_token(0, 5), 0),
            "缺 ENABLE 不算授权")
    r.check(not proto_frame_requests_arm(CTRL_FLAG_ENABLE | proto_flags_set_token(0, 5), 0),
            "缺 ARM 不算授权")
    r.check(not proto_frame_requests_arm(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 0),
            "令牌 0 非法")

    a = ArmState()
    r.check(not a.armed, "上电默认未授权")
    r.check(a.update(tok5), "首次合法授权帧 -> armed")
    r.eq(a.last_token, 5, "记录已接受令牌 5")
    r.check(a.update(tok5), "同令牌后续帧保持 armed（不是新授权）")
    a.force_safe()
    r.check(not a.armed, "force_safe 撤销 armed")
    r.eq(a.last_token, 5, "force_safe 不清 last_token")
    r.check(not a.update(tok5), "force_safe 后旧令牌 5 重放被拒")
    tok6 = proto_flags_set_token(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM, 6)
    r.check(a.update(tok6), "用户重新使能（新令牌 6）-> 再次 armed")
    r.check(not a.update(CTRL_FLAG_ENABLE | CTRL_FLAG_ARM | CTRL_FLAG_ESTOP
                         | proto_flags_set_token(0, 7)),
            "ESTOP 帧优先撤销授权")
    r.check(not a.armed, "ESTOP 后 armed == false")

    # 桥接注入的 FAILSAFE 帧：只有 bit5，没有 ENABLE/ARM/ESTOP
    fs = encode_ctrl(seq=99, flags=CTRL_FLAG_FAILSAFE, mode=MOTOR_MODE_MIT,
                     yaw_deg=0, pitch_deg=0)
    fs_dec = decode_ctrl(fs)
    r.check(fs_dec is not None, "FAILSAFE 失能帧可被解码")
    if fs_dec:
        r.check(fs_dec["failsafe"], "解码出 FAILSAFE 位")
        r.check(not fs_dec["enable"] and not fs_dec["arm"] and not fs_dec["estop"],
                "FAILSAFE 帧不含 ENABLE/ARM/ESTOP（只注入失能）")
        r.eq(fs_dec["token"], 0, "FAILSAFE 帧令牌为 0")
        r.check(not proto_frame_requests_arm(fs_dec["flags"], 0), "FAILSAFE 帧不可能触发授权")
    r.check((fs_dec["flags"] if fs_dec else 0) & CTRL_SAFETY_BITS,
            "FAILSAFE 命中安全帧判定（走 safety 队列）")
    armed = ArmState()
    armed.update(tok5)
    r.check(not armed.update(CTRL_FLAG_FAILSAFE), "FAILSAFE 帧让 STM32 撤销授权")
    r.check(not armed.armed, "FAILSAFE 后 armed == false")

    # 所有安全类 flags 都命中安全帧判定
    for bit, name in [(CTRL_FLAG_ESTOP, "ESTOP"), (CTRL_FLAG_CLEAR_ERR, "CLEAR_ERR"),
                      (CTRL_FLAG_SET_ZERO, "SET_ZERO"), (CTRL_FLAG_FAILSAFE, "FAILSAFE")]:
        r.check((CTRL_FLAG_ENABLE | bit) & CTRL_SAFETY_BITS,
                "安全帧判定包含 %s" % name)
    r.check(not (CTRL_FLAG_ENABLE & CTRL_SAFETY_BITS),
            "普通 ENABLE 帧不算安全帧（走 cmd 队列）")


def test_bridge_status(r: Runner) -> None:
    r.section("桥接新鲜度帧 0x07（契约 §1.2 字段逐字节）")
    flags = (PROTO_BR_FLAG_STM_FRESH | PROTO_BR_FLAG_HAVE_STM | PROTO_BR_FLAG_UPLINK_READY)
    frame = encode_bridge_status(
        seq=9, flags=flags, stm_age_10ms=12, stm_state=LINK_STATE_OK, stm_err=1,
        rx_ok_l8=200, rx_dup_l8=3, rx_drop_l8=1, fwd_ok_l8=198,
        failsafe_l8=5, tx_fail_l8=2, arm_reject_l8=4)

    r.eq(len(frame), BRIDGE_STATUS_FRAME_SIZE, "帧长 17 B")
    expect_low = [
        (0, PROTO_HEAD), (1, 13), (2, PROTO_MSG_BRIDGE_STATUS), (3, 9), (4, flags),
        (5, 12), (6, LINK_STATE_OK), (7, 1), (8, 200), (9, 3), (10, 1),
        (11, 198), (12, 5), (13, 2), (14, 4),
    ]
    for off, val in expect_low:
        r.eq(frame[off], val, "byte[%d] == %d" % (off, val))
    r.eq(frame[15], crc16_modbus(frame[1:15]) & 0xFF, "[15] CRC 覆盖 [1..14]")
    r.eq(frame[16], PROTO_TAIL, "[16] 帧尾 0x55")

    dec = decode_bridge_status(frame)
    r.check(dec is not None, "往返解析成功")
    if dec:
        r.eq(dec["flags"], flags, "往返 flags")
        r.eq(dec["stm_age_10ms"], 12, "往返 stm_age_10ms")
        r.eq(dec["stm_age_ms"], 120, "年龄换算 12*10 == 120 ms")
        r.eq(dec["stm_state"], LINK_STATE_OK, "往返 stm_state")
        r.eq(dec["stm_err"], 1, "往返 stm_err")
        r.eq(dec["rx_ok_l8"], 200, "往返 rx_ok_l8")
        r.eq(dec["rx_dup_l8"], 3, "往返 rx_dup_l8")
        r.eq(dec["rx_drop_l8"], 1, "往返 rx_drop_l8")
        r.eq(dec["fwd_ok_l8"], 198, "往返 fwd_ok_l8")
        r.eq(dec["failsafe_l8"], 5, "往返 failsafe_l8")
        r.eq(dec["tx_fail_l8"], 2, "往返 tx_fail_l8")
        r.eq(dec["arm_reject_l8"], 4, "往返 arm_reject_l8")

    r.eq(decode_bridge_status(encode_bridge_status(0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0))
         ["stm_age_10ms"], 255, "年龄饱和 255（2550 ms 上界）")

    for name, mutate in [
        ("CRC 错", lambda b: b.__setitem__(15, (b[15] + 1) & 0xFF)),
        ("帧尾错", lambda b: b.__setitem__(16, 0x00)),
        ("帧头错", lambda b: b.__setitem__(0, 0x00)),
        ("len 错", lambda b: b.__setitem__(1, 14)),
        ("type 被当成 STATUS(0x02)", lambda b: b.__setitem__(2, PROTO_MSG_STATUS)),
    ]:
        bad = bytearray(frame)
        mutate(bad)
        r.check(decode_bridge_status(bytes(bad)) is None, "0x07 坏帧拒绝：" + name)
    r.check(decode_bridge_status(frame[:-1]) is None, "0x07 截断帧拒绝")

    # 旧主机把 0x07 当未知类型忽略：这里用"类型不符即失败"来保证不会误当状态帧
    old_status_frame = bytearray(frame)
    old_status_frame[2] = PROTO_MSG_STATUS
    old_status_frame[15] = crc16_modbus(bytes(old_status_frame[1:15])) & 0xFF
    r.check(decode_bridge_status(bytes(old_status_frame)) is None,
            "0x07 帧不允许被 0x02 解码器接受（旧主机只会忽略，不会误读）")

    # flags 位值互不重叠
    all_flags = [PROTO_BR_FLAG_STM_FRESH, PROTO_BR_FLAG_FAILSAFE, PROTO_BR_FLAG_HAVE_STM,
                 PROTO_BR_FLAG_MOTION_BLOCKED, PROTO_BR_FLAG_UPLINK_READY,
                 PROTO_BR_FLAG_STM_ESTOP, PROTO_BR_FLAG_RX_OVERFLOW]
    r.eq(sum(all_flags), 0x7F, "0x07 flags 占满 bit0..6")
    for i in range(len(all_flags)):
        for j in range(i + 1, len(all_flags)):
            r.check((all_flags[i] & all_flags[j]) == 0, "0x07 flags bit 不重叠 (%d/%d)" % (i, j))


def test_motor_ext2(r: Runner) -> None:
    r.section("电机扩展帧 p12..14（安全位 / 丢弃计数 / ARM 回显）")
    safety = PROTO_MOTOR_SAFETY_ARMED | PROTO_MOTOR_SAFETY_ENABLED
    frame = encode_motor_ext2(seq=33, pos=1.23, vel=-4.56, tau=0.789, can_fail=7,
                              safety=safety, cmd_drops=9, arm_echo=0x42)
    r.eq(frame[2], PROTO_MSG_MOTOR, "type == 0x06")
    r.eq(frame[12], 0x03, "p[12] safety == ARMED|ENABLED")
    r.eq(frame[13], 9, "p[13] 过期/丢弃计数低 8 位")
    r.eq(frame[14], 0x42, "p[14] ARM 令牌回显")

    dec = decode_motor_ext2(frame)
    r.check(dec is not None, "往返解析成功")
    if dec:
        r.near(dec["pos"], 1.23, MOTOR_POS_SCALE * 1.5, "往返 pos（0.01 rad 量化）")
        r.near(dec["vel"], -4.56, MOTOR_VEL_SCALE * 1.5, "往返 vel")
        r.near(dec["tau"], 0.789, MOTOR_TAU_SCALE * 1.5, "往返 tau")
        r.eq(dec["can_fail"], 7, "往返 can_fail")
        r.eq(dec["safety"], safety, "往返 safety")
        r.eq(dec["cmd_drops"], 9, "往返 cmd_drops")
        r.eq(dec["arm_echo"], 0x42, "往返 arm_echo")

    # 旧解码器 proto_decode_motor() 只读到 p11，p12..14 是新增字节，不受影响
    for off in (12, 13, 14):
        legacy = bytearray(frame)
        legacy[off] = 0x00
        legacy[15] = crc16_modbus(bytes(legacy[1:15])) & 0xFF
        d = decode_motor_ext2(bytes(legacy))
        r.check(d is not None, "p[%d] 可变化且 CRC 重新成立（旧解码器忽略该字节）" % off)

    # p12..14 被 CRC 覆盖：翻转必须被拒
    for off in (12, 13, 14):
        bad = bytearray(frame)
        bad[off] ^= 0x01
        r.check(decode_motor_ext2(bytes(bad)) is None, "p[%d] 翻转 -> CRC 拒绝" % off)

    bits = [PROTO_MOTOR_SAFETY_ARMED, PROTO_MOTOR_SAFETY_ENABLED, PROTO_MOTOR_SAFETY_ESTOP,
            PROTO_MOTOR_SAFETY_FAULT, PROTO_MOTOR_SAFETY_FBK_STALE,
            PROTO_MOTOR_SAFETY_CAN_RECOVER, PROTO_MOTOR_SAFETY_LINK_LOST,
            PROTO_MOTOR_SAFETY_ARM_REJECT]
    r.eq(bits, [0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80],
         "8 个安全位是 bit0..7，各占一位")
    r.eq(sum(bits), 0xFF, "8 个安全位或起来 == 0xFF")
    for i in range(len(bits)):
        for j in range(i + 1, len(bits)):
            r.check((bits[i] & bits[j]) == 0, "电机安全位不重叠 (%d/%d)" % (i, j))

    clamped = decode_motor_ext2(encode_motor_ext2(0, 9999.0, -9999.0, 9999.0, 0, 0, 0, 0))
    if clamped:
        r.near(clamped["pos"], 327.67, 0.011, "pos 超限夹到 int16 上界 327.67 rad")
        r.near(clamped["vel"], -327.68, 0.011, "vel 超限夹到下界")
        r.near(clamped["tau"], 32.767, 0.0011, "tau 超限夹到上界")


def test_seq_window(r: Runner) -> None:
    r.section("8 bit 序号窗口（首帧 / 重复 / 乱序 / 回绕 / 128 窗口）")
    r.eq(seq_cmp8(1, 0), 1, "1 比 0 新")
    r.eq(seq_cmp8(0, 1), -1, "0 比 1 旧")
    r.eq(seq_cmp8(5, 5), 0, "相同序号 == 0")
    r.eq(seq_cmp8(0, 255), 1, "回绕后 0 比 255 新")
    r.eq(seq_cmp8(255, 0), -1, "255 比 0 旧")
    r.eq(seq_cmp8(128, 0), -1, "窗口边界：128 视为旧")
    r.eq(seq_cmp8(127, 0), 1, "窗口边界：127 视为新")

    g = SeqGate()
    r.check(g.accept(100), "首帧 100 接受")
    r.check(not g.accept(100), "重复 100 拒绝")
    r.check(g.accept(101), "101 接受")
    r.check(not g.accept(99), "乱序 99 拒绝")
    r.check(g.accept(102), "102 接受")
    r.check(not g.accept(101), "迟到 101（小于窗口内最新 102）拒绝")
    r.eq(g.duplicate, 1, "重复计数 == 1")
    r.eq(g.reorder, 2, "乱序计数 == 2（99 与迟到 101）")

    g = SeqGate()
    for s in (254, 255, 0, 1):
        r.check(g.accept(s), "回绕序列接受 seq=%d" % s)
    r.check(not g.accept(253), "回绕后迟到 253 拒绝")
    r.check(g.accept(2), "回绕后新序号 2 继续接受")

    g = SeqGate()
    r.check(g.accept(0), "从 0 开始接受")
    r.check(not g.accept(128), "相对 last=0，128 落在 128 窗口外（视为旧）拒绝")
    r.check(g.accept(127), "相对 last=0，127 在窗口内接受")
    r.eq(g.last, 127, "窗口内新序号推进 last")


def test_cmd_expiry(r: Runner) -> None:
    r.section("命令过期模型（100 ms 上限 / 毫秒回绕 / 安全优先 / 停止屏障）")
    r.eq(CTRL_CMD_MAX_AGE_MS, 100, "CTRL_CMD_MAX_AGE_MS == 100")
    r.eq(STM_CMD_MAX_AGE_MS, 100, "STM_CMD_MAX_AGE_MS == 100")
    r.check(not age_exceeds(now_ms=1000, rx_ms=901, limit_ms=100),
            "99 ms 未过期（边界内）")
    r.check(not age_exceeds(now_ms=1000, rx_ms=900, limit_ms=100),
            "恰好 100 ms 不算过期（实现用 >）")
    r.check(age_exceeds(now_ms=1000, rx_ms=899, limit_ms=100),
            "101 ms 过期，必须丢弃")
    r.check(not age_exceeds(now_ms=0x20, rx_ms=0xFFFFFFF0, limit_ms=100),
            "32 位毫秒回绕：48 ms 未过期")
    r.check(age_exceeds(now_ms=0x100, rx_ms=0xFFFFFFF0, limit_ms=100),
            "32 位毫秒回绕：272 ms 过期")

    q = CmdQueue()
    gate = SeqGate()
    q.push(seq=1, rx_ms=0, safety=False)
    q.push(seq=2, rx_ms=50, safety=True)
    r.check(q.pop(now_ms=400, max_age_ms=100, gate=gate) is None,
            "全部超时 -> 不执行任何命令")
    r.eq(q.expired, 2, "过期计数 == 2")

    q = CmdQueue()
    gate = SeqGate()
    q.push(seq=11, rx_ms=390, safety=False)   # 运动命令（序号较新）
    q.push(seq=10, rx_ms=395, safety=True)    # 安全命令（序号较旧、后入队）
    first = q.pop(now_ms=400, max_age_ms=100, gate=gate)
    r.check(first is not None and first["safety"], "安全命令优先出队")
    r.eq(first["seq"] if first else None, 10, "先执行安全命令 seq=10")
    second = q.pop(now_ms=400, max_age_ms=100, gate=gate)
    r.eq(second["seq"] if second else None, 11, "再执行运动命令 seq=11")

    q = CmdQueue()
    gate = SeqGate()
    q.push(seq=20, rx_ms=350, safety=False)
    r.check(q.pop(now_ms=460, max_age_ms=100, gate=gate) is None,
            "过期运动命令不执行（即使队列里只剩它）")
    q.push(seq=21, rx_ms=450, safety=True)
    got = q.pop(now_ms=460, max_age_ms=100, gate=gate)
    r.eq(got["seq"] if got else None, 21, "过期帧跳过后继续取到新鲜安全命令")

    q = CmdQueue()
    gate = SeqGate()
    q.push(seq=30, rx_ms=500, safety=False)
    q.push(seq=30, rx_ms=500, safety=False)
    r.check(q.pop(now_ms=500, max_age_ms=100, gate=gate) is not None, "重复序号第一帧执行")
    r.check(q.pop(now_ms=500, max_age_ms=100, gate=gate) is None, "重复序号第二帧丢弃")

    q = CmdQueue()
    q.push(seq=40, rx_ms=0, safety=False)
    q.push(seq=41, rx_ms=0, safety=False)
    q.push(seq=42, rx_ms=0, safety=True)
    r.eq(q.purge_motion(), 2, "停止屏障清掉 2 条运动命令")
    r.eq(len(q.items), 1, "安全命令保留 1 条")
    r.eq(q.items[0]["seq"], 42, "保留的是安全命令")


def test_timeouts_doc(r: Runner) -> None:
    r.section("契约 §2 保护时限常量（文档/测试同一口径）")
    table = [
        ("CTRL_CMD_MAX_AGE_MS", CTRL_CMD_MAX_AGE_MS, 100),
        ("BRIDGE_MASTER_TIMEOUT_MS", BRIDGE_MASTER_TIMEOUT_MS, 300),
        ("BRIDGE_FAILSAFE_PERIOD_MS", BRIDGE_FAILSAFE_PERIOD_MS, 50),
        ("BRIDGE_STM_FRESH_MS", BRIDGE_STM_FRESH_MS, 300),
        ("BRIDGE_UPLINK_PERIOD_MS", BRIDGE_UPLINK_PERIOD_MS, 100),
        ("STM_CTRL_TIMEOUT_MS", STM_CTRL_TIMEOUT_MS, 300),
        ("STM_CMD_MAX_AGE_MS", STM_CMD_MAX_AGE_MS, 100),
        ("STM_FB_TIMEOUT_MS", STM_FB_TIMEOUT_MS, 200),
        ("STM_FB_GRACE_MS", STM_FB_GRACE_MS, 1000),
        ("MASTER_STM_STALE_MS", MASTER_STM_STALE_MS, 300),
        ("MASTER_BRIDGE_STALE_MS", MASTER_BRIDGE_STALE_MS, 300),
        ("ESPNOW_CB_TIMEOUT_MS", ESPNOW_CB_TIMEOUT_MS, 250),
        ("WS_CMD_Q_LEN", WS_CMD_Q_LEN, 8),
        ("CAN_RECOVER_THRESHOLD", CAN_RECOVER_THRESHOLD, 64),
        ("CAN_RECOVER_PERIOD_MS", CAN_RECOVER_PERIOD_MS, 2000),
    ]
    for name, got, expect in table:
        r.eq(got, expect, "%s == %d（契约 §2）" % (name, expect))


def test_header_cross_check(r: Runner) -> None:
    """只读交叉核对 protocol.h 的 v1.1 常量（不修改固件）。

    这是"第三种实现"的价值所在：如果 C 头的帧类型/位定义被人改错，
    这里会立刻对不上。找不到头文件时跳过而不是失败（方便单独拷走脚本用）。
    """
    r.section("与 firmware/lib/protocol/protocol.h 只读交叉核对")
    here = Path(__file__).resolve()
    candidates = [
        here.parent.parent / "firmware" / "lib" / "protocol" / "protocol.h",
        Path.cwd() / "firmware" / "lib" / "protocol" / "protocol.h",
    ]
    header = next((p for p in candidates if p.is_file()), None)
    if header is None:
        print("  [SKIP] 未找到 protocol.h（脚本被单独拷走时正常），跳过交叉核对")
        return

    text = header.read_text(encoding="utf-8", errors="replace")

    def define_int(name: str):
        m = re.search(r"#define\s+" + re.escape(name) + r"\s+\(?\s*(0x[0-9A-Fa-f]+|\d+)u?\s*\)?", text)
        if not m:
            # 枚举值（如 PROTO_MSG_BRIDGE_STATUS = 0x07）也支持
            m = re.search(r"\b" + re.escape(name) + r"\s*=\s*(0x[0-9A-Fa-f]+|\d+)", text)
        return int(m.group(1), 0) if m else None

    checks = [
        ("PROTO_MSG_BRIDGE_STATUS", 0x07),
        ("PROTO_HEAD", PROTO_HEAD),
        ("PROTO_TAIL", PROTO_TAIL),
        ("PROTO_STX", PROTO_STX),
        ("CTRL_FLAG_ENABLE", CTRL_FLAG_ENABLE),
        ("CTRL_FLAG_CLEAR_ERR", CTRL_FLAG_CLEAR_ERR),
        ("CTRL_FLAG_SET_ZERO", CTRL_FLAG_SET_ZERO),
        ("CTRL_FLAG_ESTOP", CTRL_FLAG_ESTOP),
        ("CTRL_FLAG_ARM", CTRL_FLAG_ARM),
        ("CTRL_FLAG_FAILSAFE", CTRL_FLAG_FAILSAFE),
        ("CTRL_FLAG_ARM_TOKEN_MASK", CTRL_FLAG_ARM_TOKEN_MASK),
        ("PROTO_BR_FLAG_STM_FRESH", PROTO_BR_FLAG_STM_FRESH),
        ("PROTO_BR_FLAG_FAILSAFE", PROTO_BR_FLAG_FAILSAFE),
        ("PROTO_BR_FLAG_HAVE_STM", PROTO_BR_FLAG_HAVE_STM),
        ("PROTO_BR_FLAG_MOTION_BLOCKED", PROTO_BR_FLAG_MOTION_BLOCKED),
        ("PROTO_BR_FLAG_UPLINK_READY", PROTO_BR_FLAG_UPLINK_READY),
        ("PROTO_BR_FLAG_STM_ESTOP", PROTO_BR_FLAG_STM_ESTOP),
        ("PROTO_BR_FLAG_RX_OVERFLOW", PROTO_BR_FLAG_RX_OVERFLOW),
        ("PROTO_MOTOR_SAFETY_ARMED", PROTO_MOTOR_SAFETY_ARMED),
        ("PROTO_MOTOR_SAFETY_ENABLED", PROTO_MOTOR_SAFETY_ENABLED),
        ("PROTO_MOTOR_SAFETY_ESTOP", PROTO_MOTOR_SAFETY_ESTOP),
        ("PROTO_MOTOR_SAFETY_FAULT", PROTO_MOTOR_SAFETY_FAULT),
        ("PROTO_MOTOR_SAFETY_FBK_STALE", PROTO_MOTOR_SAFETY_FBK_STALE),
        ("PROTO_MOTOR_SAFETY_CAN_RECOVER", PROTO_MOTOR_SAFETY_CAN_RECOVER),
        ("PROTO_MOTOR_SAFETY_LINK_LOST", PROTO_MOTOR_SAFETY_LINK_LOST),
        ("PROTO_MOTOR_SAFETY_ARM_REJECT", PROTO_MOTOR_SAFETY_ARM_REJECT),
    ]
    for name, expect in checks:
        got = define_int(name)
        if got is None:
            print("  [WARN] protocol.h 缺少 #define %s（可能尚未合入 v1.1）" % name)
        else:
            r.eq(got, expect, "protocol.h %s == 0x%X" % (name, expect))

    # 帧长常量
    for name, expect in [("CTRL_FRAME_SIZE", 17), ("STATUS_FRAME_SIZE", 17),
                         ("BRIDGE_STATUS_FRAME_SIZE", 17)]:
        got = define_int(name)
        if got is None:
            print("  [WARN] protocol.h 缺少 #define %s" % name)
        else:
            r.eq(got, expect, "protocol.h %s == %d" % (name, expect))


# ============================================================================
# 五、入口
# ============================================================================

def main() -> int:
    ap = argparse.ArgumentParser(description="靶车协议 v1.1 Python 独立对拍测试")
    ap.add_argument("-v", "--verbose", action="store_true", help="打印每条断言")
    args = ap.parse_args()

    print("=" * 72)
    print("  靶车协议 v1.1 · Python 独立实现 对拍 / 行为模型测试")
    print("  对照: firmware/lib/protocol/protocol.h + 契约 §1/§2")
    print("=" * 72)

    r = Runner(args.verbose)
    for fn in (test_crc, test_ctrl_frame, test_arm_token, test_bridge_status,
               test_motor_ext2, test_seq_window, test_cmd_expiry,
               test_timeouts_doc, test_header_cross_check):
        try:
            fn(r)
        except Exception as exc:  # 测试代码自身异常也要明确失败，不能静默通过
            r.failed += 1
            print("  [FAIL] %s 执行异常: %r" % (fn.__name__, exc))

    total = r.passed + r.failed
    print("\n" + "=" * 72)
    if r.failed == 0:
        print("  全部通过: %d/%d 项断言 [OK]" % (r.passed, total))
        print("  Python 独立实现与 v1.1 协议一致；协议兼容性/时限常量已锁定。")
        print("=" * 72)
        return 0
    print("  失败 %d 项 / 共 %d 项 [FAIL]" % (r.failed, total))
    print("  -> 说明协议实现与契约 v1.1 不一致，禁止只升级一端就上板。")
    print("=" * 72)
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(2)
