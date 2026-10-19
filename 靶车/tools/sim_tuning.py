#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MIT 阻抗控制 * 参数手感仿真 (不接硬件)
====================================================================

把靶车这套控制链建出来跑一遍, 让你在烧板子之前就看清 kp / kd 的作用:

  电机侧:  1 kHz 执行  tau = kp*(theta目标-theta) + kd*(omega目标-omega) + tau前馈
  机械侧:  J*thetä = tau - b*thetȧ
  指令侧:  目标每 20 ms (50 Hz) 更新一次, 和 ESP-NOW 的实际节奏一致

输出每个参数组合的:
  - 阻尼比 zeta = kd / (2*sqrt(kp*J))      <- 预测冲不冲 / 抖不抖
  - 自然频率 f_n = sqrt(kp/J) / 2pi       <- 预测响应快慢
  - 实际过冲量 (%)                        <- 仿真结果, 用来验证 zeta 的预测
  - 稳定时间 (s)                          <- 进入 +/-2% 误差带的时间

用法:
    python tools/sim_tuning.py
    python tools/sim_tuning.py --kp 80 --kd 2      # 看单组参数
    python tools/sim_tuning.py --plot --kp 60 --kd 1.5   # 画 ASCII 响应曲线
"""

from __future__ import annotations

import argparse
import math

# ---------------------------------------------------------------------------
# 被控对象参数 —— 改这里匹配你的真实云台
# ---------------------------------------------------------------------------
J_DEFAULT = 0.01      # 转动惯量 kg*m^2  (靶板 ~0.5kg @ 0.1m 半径 估算)
B_FRICTION = 0.02     # 粘滞摩擦 N*m/(rad/s), 真实系统都有, 也帮助稳定

DT_CTRL = 0.001       # 控制周期 1 kHz  (STM32 的 TIM2)
DT_CMD = 0.020        # 指令周期 50 Hz  (ESP-NOW 下发节奏)

STEP_ANGLE_DEG = 30.0
SIM_DURATION = 3.0


def damping_ratio(kp: float, kd: float, J: float) -> float:
    """阻尼比 zeta = kd / (2*sqrt(kp*J))"""
    return kd / (2.0 * math.sqrt(kp * J))


def natural_freq_hz(kp: float, J: float) -> float:
    """无阻尼自然频率 f_n = sqrt(kp/J) / 2pi"""
    return math.sqrt(kp / J) / (2.0 * math.pi)


def simulate(kp: float, kd: float, J: float,
             duration: float = SIM_DURATION, cmd_vel: float = 0.0):
    """
    仿真一次阶跃响应。
    cmd_vel 是"目标速度"(速度前馈), 阶跃时给 0; 斜坡输入时可给非 0。
    返回 (时间序列角度(度), 峰值, 稳定时间)
    """
    target = math.radians(STEP_ANGLE_DEG)

    theta = 0.0      # 当前角度 rad
    omega = 0.0      # 当前角速度 rad/s
    t = 0.0

    hist = []
    settle_time = None
    peak = 0.0
    steps = int(duration / DT_CTRL)

    # 用一个共享的 "指令锁存" 模拟 50Hz 更新: 每 20 个控制周期更新一次
    cmd_update_every = max(1, int(round(DT_CMD / DT_CTRL)))
    cmd_theta = 0.0
    cmd_omega = 0.0

    for i in range(steps):
        # ---- 50 Hz 指令更新 ----
        if i % cmd_update_every == 0:
            cmd_theta = target
            cmd_omega = cmd_vel

        # ---- 1 kHz MIT 控制律 ----
        tau = kp * (cmd_theta - theta) + kd * (cmd_omega - omega)

        # 力矩限幅: DM6220 的 TMAX = +/-10 N*m
        tau = max(-10.0, min(10.0, tau))

        # ---- 机械动力学 ----
        alpha = (tau - B_FRICTION * omega) / J
        omega += alpha * DT_CTRL
        theta += omega * DT_CTRL
        t += DT_CTRL

        hist_err = abs(math.degrees(theta) - STEP_ANGLE_DEG)
        hist.append((t, math.degrees(theta), hist_err))
        peak = max(peak, math.degrees(theta))

    # 稳定时间 = *最后一次* 离开 +/-2% 误差带的时刻 (不是第一次进入!)
    # 否则大幅振荡的系统会因为早期穿越而得到一个虚假的"很快稳定"
    band = 0.02 * STEP_ANGLE_DEG
    settle_time = None
    for ts, _, err in hist:
        if err > band:
            settle_time = ts
    if settle_time is not None and settle_time <= DT_CMD:
        settle_time = None      # 一开始就在带外, 不算稳定

    return hist, peak, settle_time


def ascii_plot(hist, width: int = 62, height: int = 14):
    """把响应曲线画成 ASCII 图 (0..峰值 映射到 height 行)"""
    pts = hist[:: max(1, len(hist) // width)][:width]
    vals = [v for _, v, _ in pts]
    top = max(max(vals), STEP_ANGLE_DEG) * 1.05
    bot = 0.0

    grid = [[" "] * len(pts) for _ in range(height)]

    def row_of(v):
        if top == bot:
            return height - 1
        r = int(round((top - v) / (top - bot) * (height - 1)))
        return max(0, min(height - 1, r))

    # 目标线
    tr = row_of(STEP_ANGLE_DEG)
    for x in range(len(pts)):
        grid[tr][x] = "-"

    # 实际曲线
    for x, (_, v, _) in enumerate(pts):
        grid[row_of(v)][x] = "*"

    out = []
    for r in range(height):
        label = f"{top - (top - bot) * r / (height - 1):6.1f}"
        out.append(f"  {label} |" + "".join(grid[r]))
    return "\n".join(out)


def report(kp: float, kd: float, J: float, show_plot: bool = False):
    zeta = damping_ratio(kp, kd, J)
    fn = natural_freq_hz(kp, J)
    hist, peak, settle = simulate(kp, kd, J)
    overshoot = max(0.0, (peak - STEP_ANGLE_DEG) / STEP_ANGLE_DEG * 100.0)

    if zeta > 1.2:
        verdict = "过阻尼: 稳但偏迟钝"
    elif zeta >= 0.8:
        verdict = "临界附近: 稳, 推荐"
    elif zeta >= 0.5:
        verdict = "欠阻尼: 有过冲"
    elif zeta >= 0.25:
        verdict = "严重欠阻尼: 明显振荡"
    else:
        verdict = "几乎无阻尼: 必啸叫"

    settle_txt = f"{settle:.3f}s" if settle is not None else f">{SIM_DURATION:.1f}s (振荡未收敛)"
    print(f"\nkp={kp:6.0f}  kd={kd:5.2f}  J={J}  ->  zeta={zeta:5.2f}  f_n={fn:5.1f}Hz")
    print(f"    过冲 {overshoot:5.1f}%   稳定时间 {settle_txt}")
    print(f"    判定: {verdict}")

    if show_plot:
        print("\n    30 deg 阶跃响应 (虚线=目标, * =实际角度):")
        print(ascii_plot(hist))
        print(f"    峰值 {peak:.2f} deg")


def main():
    ap = argparse.ArgumentParser(description="MIT 阻抗控制参数仿真")
    ap.add_argument("--kp", type=float)
    ap.add_argument("--kd", type=float)
    ap.add_argument("--J", type=float, default=J_DEFAULT)
    ap.add_argument("--plot", action="store_true")
    args = ap.parse_args()

    print("=" * 72)
    print("  MIT 阻抗控制参数仿真")
    print(f"  J = {args.J} kg*m^2  控制 1kHz  指令 50Hz  阶跃 {STEP_ANGLE_DEG:.0f} deg")
    print("=" * 72)

    if args.kp is not None and args.kd is not None:
        report(args.kp, args.kd, args.J, args.plot)
        return 0

    # 默认: 扫一组有代表性的参数, 展示 kp / kd 各自的作用
    print("\n【第 1 组】固定 kp=60, 只改 kd —— 看阻尼比和过冲怎么变")
    print("  (这组说明: kd 是控制'冲不冲'和'抖不抖'的旋钮)")
    for kd in (0.1, 0.3, 0.8, 1.5, 3.0):
        report(60, kd, args.J)

    print("\n" + "-" * 72)
    print("【第 2 组】固定 kd=1.5, 只改 kp —— 看响应快慢和稳定裕度怎么变")
    print("  (这组说明: kp 越大越快, 但 zeta 会下降, 所以加 kp 要配着加 kd)")
    for kp in (20, 60, 150, 300):
        report(kp, 1.5, args.J)

    print("\n" + "-" * 72)
    print("【第 3 组】加 kp 时同步补 kd, 保住 zeta —— 又快又稳")
    for kp, kd in ((60, 1.5), (150, 2.4), (300, 3.4)):
        report(kp, kd, args.J)

    print("\n" + "=" * 72)
    print("  启示:")
    print("  1. zeta ~ 1 附近最好看: 不冲、不过慢。kd 每 0.5 一档调。")
    print("  2. 加 kp 会让 zeta 按 1/sqrt(kp) 下降, 所以加 kp 必须补 kd。")
    print("  3. J 变大 (装上更重的靶板) 会让 zeta 也按 1/sqrt(J) 下降,")
    print("     这就是'空载调好、装上负载就振'的根本原因。")
    print("=" * 72)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
