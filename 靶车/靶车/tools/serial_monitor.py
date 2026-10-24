#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
serial_monitor.py —— 极简串口监视器 (替代 `pio device monitor`)

只依赖 pyserial。PlatformIO 的 Python 环境里已经自带, 所以 CMake 的
`monitor` 目标会直接用 <PlatformIO>/penv/Scripts/python.exe 来跑它,
不需要你另外装任何东西。

用法:
    python serial_monitor.py                 # 自动挑一个串口
    python serial_monitor.py -p COM5         # 指定串口
    python serial_monitor.py -p COM5 --baud 460800
    python serial_monitor.py --list          # 只列出可用串口

退出: Ctrl+C
"""

import argparse
import sys


def list_ports():
    try:
        from serial.tools import list_ports as lp
    except ImportError:
        print("缺少 pyserial。用 PlatformIO 自带的 Python 即可:\n"
              "    ~/.platformio/penv/Scripts/python.exe serial_monitor.py", file=sys.stderr)
        return []
    return sorted(lp.comports(), key=lambda p: p.device)


def pick_port():
    ports = list_ports()
    if not ports:
        return None
    if len(ports) == 1:
        return ports[0].device
    # 常见板卡的 USB 转串口关键字, 挑更像开发板的那个
    hints = ("usb", "ch340", "ch341", "cp210", "ft23", "silicon", "stlink",
             "espressif", "m5stack", "arduino", "wch")
    for p in ports:
        blob = f"{p.device} {p.description} {p.manufacturer or ''}".lower()
        if any(h in blob for h in hints):
            return p.device
    return ports[0].device


def main():
    ap = argparse.ArgumentParser(description="串口监视器")
    ap.add_argument("-p", "--port", default=None, help="串口名, 例如 COM5")
    ap.add_argument("--baud", type=int, default=115200, help="波特率 (默认 115200)")
    ap.add_argument("--list", action="store_true", help="只列出串口后退出")
    args = ap.parse_args()

    if args.list:
        ports = list_ports()
        if not ports:
            print("没有找到任何串口。")
        for p in ports:
            print(f"  {p.device:10s}  {p.description}")
        return 0

    try:
        import serial
    except ImportError:
        print("缺少 pyserial。请用 PlatformIO 自带的 Python 运行本脚本:",
              file=sys.stderr)
        print("    %USERPROFILE%\\.platformio\\penv\\Scripts\\python.exe "
              "tools\\serial_monitor.py -p COM5", file=sys.stderr)
        return 2

    port = args.port or pick_port()
    if not port:
        print("没有找到串口。插上板子, 或者用 -p COMx 指定。", file=sys.stderr)
        return 1

    print(f"== 已打开 {port} @ {args.baud} 8N1  (Ctrl+C 退出) ==", file=sys.stderr)
    try:
        with serial.Serial(port, args.baud, timeout=0.2) as ser:
            while True:
                data = ser.read(ser.in_waiting or 1)
                if data:
                    sys.stdout.buffer.write(data)
                    sys.stdout.buffer.flush()
    except KeyboardInterrupt:
        print("\n== 已退出 ==", file=sys.stderr)
        return 0
    except serial.SerialException as exc:
        print(f"打开串口失败: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
