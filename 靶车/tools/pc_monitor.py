#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
靶车链路 · PC 端串口监视 / 对拍工具
====================================================================

这是 protocol.h 的一份独立 Python 实现, 用途有两个:

  1. 【对拍】用另一门语言独立重算帧格式和 CRC, 和 C 实现互相印证。
     跑 `python tools/pc_monitor.py --selftest`, 会验证 CRC16-Modbus 的
     标准向量并生成/回读示例帧。三方固件都信 protocol.h, 万一有人改错了,
     这个脚本能第一时间发现两侧对不上。

  2. 【现场调试】接任意一端 (建议接主机 ESP32 的 USB), 实时解析链路上的
     原始字节, 把人类看不懂的二进制帧翻译成角度/标志/序号/CRC 校验结果。
     排查"到底是没发出来、CRC 错、还是角度不对"这类问题非常快。

用法:
    python tools/pc_monitor.py --selftest                  # 只跑对拍, 不接硬件
    python tools/pc_monitor.py -p COM5                     # 监视主机串口 (文本日志)
    python tools/pc_monitor.py -p COM6 --mode bridge       # 监视桥接端 (含反馈帧)
    python tools/pc_monitor.py -p COM5 --raw               # 同时打印原始 hex

依赖: pyserial (只有接硬件时才需要)
    pip install pyserial
"""

from __future__ import annotations

import argparse
import struct
import sys
import time

# ----------------------------------------------------------------------------
# 协议常量 —— 必须与 firmware/lib/protocol/protocol.h 保持一致
# ----------------------------------------------------------------------------

PROTO_HEAD          = 0xAA
PROTO_TAIL          = 0x55
PROTO_STX           = 0x5A

PROTO_MSG_CTRL      = 0x01
PROTO_MSG_STATUS    = 0x02

CTRL_FRAME_SIZE     = 17
STATUS_FRAME_SIZE   = 17

PROTO_ANGLE_SCALE   = 0.01
PROTO_RATE_SCALE    = 0.1

CTRL_FLAG_ENABLE    = 0x0001
CTRL_FLAG_CLEAR_ERR = 0x0002
CTRL_FLAG_SET_ZERO  = 0x0004
CTRL_FLAG_ESTOP     = 0x0008

MOTOR_MODE_NAMES = {0: "MIT", 1: "POS", 2: "SPD", 3: "PSI"}

LINK_STATE_NAMES = {
    0: "INIT",
    1: "OK",
    2: "TIMEOUT(失联)",
    3: "ESTOP(急停)",
    4: "FAULT(故障)",
}


def crc16_modbus(data: bytes) -> int:
    """CRC16-Modbus, 与 protocol.h 的 proto_crc16 逐位一致"""
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            if crc & 1:
                crc = (crc >> 1) ^ 0xA001
            else:
                crc >>= 1
    return crc


# ----------------------------------------------------------------------------
# 帧解析
# ----------------------------------------------------------------------------

def decode_ctrl_frame(frame: bytes):
    """解析 17 字节控制帧; 校验失败返回 None"""
    if len(frame) != CTRL_FRAME_SIZE:
        return None
    if frame[0] != PROTO_HEAD or frame[1] != 13 or frame[2] != PROTO_MSG_CTRL:
        return None
    if frame[16] != PROTO_TAIL:
        return None
    if (crc16_modbus(frame[1:15]) & 0xFF) != frame[15]:
        return None

    flags = frame[4] | (frame[5] << 8)

    def s16(lo: int, hi: int) -> int:
        v = frame[lo] | (frame[hi] << 8)
        return v - 0x10000 if v & 0x8000 else v

    return {
        "seq": frame[3],
        "flags": flags,
        "enable": bool(flags & CTRL_FLAG_ENABLE),
        "clear_err": bool(flags & CTRL_FLAG_CLEAR_ERR),
        "set_zero": bool(flags & CTRL_FLAG_SET_ZERO),
        "estop": bool(flags & CTRL_FLAG_ESTOP),
        "mode": MOTOR_MODE_NAMES.get(frame[6], f"?{frame[6]}"),
        "yaw_deg": s16(7, 8) * PROTO_ANGLE_SCALE,
        "pitch_deg": s16(9, 10) * PROTO_ANGLE_SCALE,
        "yaw_rate_dps": s16(11, 12) * PROTO_RATE_SCALE,
        "pitch_rate_dps": s16(13, 14) * PROTO_RATE_SCALE,
    }


def decode_status_frame(frame: bytes):
    """解析 17 字节反馈帧; 校验失败返回 None"""
    if len(frame) != STATUS_FRAME_SIZE:
        return None
    if frame[0] != PROTO_STX or frame[1] != 15 or frame[2] != PROTO_MSG_STATUS:
        return None
    if (crc16_modbus(frame[1:16]) & 0xFF) != frame[16]:
        return None

    yaw, pitch = struct.unpack_from("<ff", frame, 6)
    return {
        "seq": frame[3],
        "rssi": frame[4],
        "state": LINK_STATE_NAMES.get(frame[5], f"?{frame[5]}"),
        "yaw_deg": yaw,
        "pitch_deg": pitch,
        "temp_mos": frame[14],
        "err_code": frame[15],
    }


# ----------------------------------------------------------------------------
# 自测 (对拍)
# ----------------------------------------------------------------------------

def selftest() -> int:
    print("=" * 68)
    print("  靶车协议 · PC 端独立实现对拍")
    print("=" * 68)
    failures = 0

    def check(cond, msg):
        nonlocal failures
        print(f"  [{'PASS' if cond else 'FAIL'}] {msg}")
        if not cond:
            failures += 1

    # 1. CRC 标准向量
    crc = crc16_modbus(b"123456789")
    print(f"\n== CRC16-Modbus ==\n  CRC(\"123456789\") = 0x{crc:04X}")
    check(crc == 0x4B37, "标准向量 0x4B37")
    check(crc16_modbus(b"") == 0xFFFF, "空数据初值 0xFFFF")

    # 2. 手工构造一帧控制帧, 再用解析器读回来
    print("\n== 控制帧 编/解 ==")
    yaw_q = int(round(12.34 / PROTO_ANGLE_SCALE))      # 1234
    pitch_q = int(round(-45.67 / PROTO_ANGLE_SCALE))   # -4567
    yr_q = int(round(88.0 / PROTO_RATE_SCALE))         # 880
    pr_q = int(round(-12.5 / PROTO_RATE_SCALE))        # -125
    flags = CTRL_FLAG_ENABLE

    frame = bytearray(CTRL_FRAME_SIZE)
    frame[0] = PROTO_HEAD
    frame[1] = 13
    frame[2] = PROTO_MSG_CTRL
    frame[3] = 7
    frame[4] = flags & 0xFF
    frame[5] = (flags >> 8) & 0xFF
    frame[6] = 0
    struct.pack_into("<hh", frame, 7, yaw_q, pitch_q)
    struct.pack_into("<hh", frame, 11, yr_q, pr_q)
    frame[15] = crc16_modbus(bytes(frame[1:15])) & 0xFF
    frame[16] = PROTO_TAIL

    print(f"  构造帧: {frame.hex(' ').upper()}")
    dec = decode_ctrl_frame(bytes(frame))
    check(dec is not None, "控制帧解析成功")
    if dec:
        check(dec["seq"] == 7, "seq 正确")
        check(dec["enable"] and dec["mode"] == "MIT", "flags/mode 正确")
        check(abs(dec["yaw_deg"] - 12.34) < 0.011, f"yaw={dec['yaw_deg']:.2f} 度")
        check(abs(dec["pitch_deg"] + 45.67) < 0.011, f"pitch={dec['pitch_deg']:.2f} 度")
        check(abs(dec["yaw_rate_dps"] - 88.0) < 0.11, f"yaw_rate={dec['yaw_rate_dps']:.1f} dps")

    # 3. 坏帧必须被拒
    print("\n== 坏帧拒绝 ==")
    bad = bytearray(frame); bad[9] ^= 0x01
    check(decode_ctrl_frame(bytes(bad)) is None, "数据位翻转 -> CRC 拒绝")
    bad = bytearray(frame); bad[0] = 0xAB
    check(decode_ctrl_frame(bytes(bad)) is None, "帧头错 -> 拒绝")
    bad = bytearray(frame); bad[16] = 0x00
    check(decode_ctrl_frame(bytes(bad)) is None, "帧尾错 -> 拒绝")
    check(decode_ctrl_frame(bytes(frame)[:-1]) is None, "截断帧 -> 拒绝")

    # 4. 反馈帧
    print("\n== 反馈帧 编/解 ==")
    st = bytearray(STATUS_FRAME_SIZE)
    st[0] = PROTO_STX
    st[1] = 15
    st[2] = PROTO_MSG_STATUS
    st[3] = 42
    st[4] = 55
    st[5] = 1
    struct.pack_into("<ff", st, 6, -33.25, 7.75)
    st[14] = 46
    st[15] = 0
    st[16] = crc16_modbus(bytes(st[1:16])) & 0xFF

    print(f"  构造帧: {st.hex(' ').upper()}")
    dec = decode_status_frame(bytes(st))
    check(dec is not None, "反馈帧解析成功")
    if dec:
        check(dec["seq"] == 42 and dec["rssi"] == 55, "seq/rssi 正确")
        check(dec["state"] == "OK", f"state={dec['state']}")
        check(abs(dec["yaw_deg"] + 33.25) < 1e-4, f"yaw={dec['yaw_deg']:.2f} 度")
        check(abs(dec["pitch_deg"] - 7.75) < 1e-4, f"pitch={dec['pitch_deg']:.2f} 度")
        check(dec["temp_mos"] == 46 and dec["err_code"] == 0, "温度/错误码 正确")

    bad = bytearray(st); bad[10] ^= 0x80
    check(decode_status_frame(bytes(bad)) is None, "反馈帧篡改 -> 拒绝")

    print("\n" + "=" * 68)
    if failures == 0:
        print("  PC 端实现与 protocol.h 一致, 对拍全部通过 [OK]")
    else:
        print(f"  失败 {failures} 项 [FAIL] —— 说明 C 端和 Python 端协议不一致!")
    print("=" * 68)
    return 0 if failures == 0 else 1


# ----------------------------------------------------------------------------
# 串口监视
# ----------------------------------------------------------------------------

def monitor(port: str, baud: int, mode: str, show_raw: bool) -> int:
    try:
        import serial  # type: ignore
    except ImportError:
        print("需要 pyserial:  pip install pyserial", file=sys.stderr)
        return 2

    frame_size = CTRL_FRAME_SIZE if mode == "master" else STATUS_FRAME_SIZE
    head = PROTO_HEAD if mode == "master" else PROTO_STX

    print(f"打开 {port} @ {baud}, 模式={mode}, 等待数据... (Ctrl+C 退出)")

    buf = bytearray()
    ok = bad = 0
    last_report = time.time()

    with serial.Serial(port, baud, timeout=0.05) as ser:
        try:
            while True:
                chunk = ser.read(256)
                if not chunk:
                    continue

                if show_raw:
                    print(f"  RAW {chunk.hex(' ').upper()}")

                buf.extend(chunk)

                # 同步到帧头
                while buf and buf[0] != head:
                    buf.pop(0)

                while len(buf) >= frame_size:
                    candidate = bytes(buf[:frame_size])
                    dec = (decode_ctrl_frame(candidate) if mode == "master"
                           else decode_status_frame(candidate))
                    if dec is None:
                        bad += 1
                        buf.pop(0)
                        continue

                    ok += 1
                    buf = buf[frame_size:]

                    if mode == "master":
                        print(f"[控制] seq={dec['seq']:3d} {dec['mode']:<3} "
                              f"yaw={dec['yaw_deg']:8.2f} pitch={dec['pitch_deg']:7.2f} "
                              f"rate=({dec['yaw_rate_dps']:7.1f},{dec['pitch_rate_dps']:7.1f}) "
                              f"{'EN ' if dec['enable'] else 'dis'} "
                              f"{'ESTOP ' if dec['estop'] else ''}"
                              f"{'CLRERR ' if dec['clear_err'] else ''}"
                              f"{'ZERO' if dec['set_zero'] else ''}")
                    else:
                        print(f"[反馈] state={dec['state']:<12} yaw={dec['yaw_deg']:8.2f} "
                              f"pitch={dec['pitch_deg']:7.2f} T={dec['temp_mos']:3d}C "
                              f"err={dec['err_code']} rssi={dec['rssi']}")

                now = time.time()
                if now - last_report >= 5.0:
                    last_report = now
                    print(f"  ---- 统计: 有效 {ok} 帧, 校验失败 {bad} 帧 ----")

        except KeyboardInterrupt:
            print(f"\n退出。共收到有效 {ok} 帧, 失败 {bad} 帧。")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="靶车链路 PC 端监视/对拍工具")
    ap.add_argument("-p", "--port", help="串口号, 如 COM5")
    ap.add_argument("-b", "--baud", type=int, default=460800, help="波特率 (默认 460800)")
    ap.add_argument("--mode", choices=["master", "bridge"], default="master",
                    help="master=主机向控制帧, bridge=桥接侧反馈帧")
    ap.add_argument("--raw", action="store_true", help="打印原始字节")
    ap.add_argument("--selftest", action="store_true", help="只跑协议对拍, 不接硬件")
    args = ap.parse_args()

    if args.selftest or not args.port:
        return selftest()
    return monitor(args.port, args.baud, args.mode, args.raw)


if __name__ == "__main__":
    sys.exit(main())
