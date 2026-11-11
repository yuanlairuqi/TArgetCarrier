#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
dm_gui.py —— 靶车控制台 (桌面版, 类似达妙调试助手那种"随时能开"的工具)

用法:
    双击 tools\\打开靶车控制台.bat
    或:  python dm_gui.py [COM5]

它做三件事:
    1. 列出串口, 连上靶车 ESP32 主机 (460800 8N1)
    2. 把主机每 500ms 打印的状态行解析成界面上的数字
    3. 界面上的按钮/滑块 = 往串口发一行命令 (和手敲控制台完全一样)

只依赖 pyserial; tkinter 用 Python 自带。如果当前解释器没有 pyserial,
脚本会自动去 PlatformIO 的 penv 里找 (纯 Python 库, 跨版本可用)。
"""

import os
import re
import sys
import json
import queue
import threading
import time
import webbrowser

try:
    import serial
    from serial.tools import list_ports
except ImportError:  # 没有 pyserial -> 借 PlatformIO 的
    _penv = os.path.expanduser(os.path.join("~", ".platformio", "penv", "Lib", "site-packages"))
    if os.path.isdir(_penv) and _penv not in sys.path:
        sys.path.append(_penv)
    try:
        import serial
        from serial.tools import list_ports
    except ImportError:
        sys.stderr.write("缺少 pyserial:  python -m pip install pyserial\n")
        raise

import tkinter as tk
from tkinter import ttk, messagebox

BAUD = 460800

# 主机 print_status() 的输出, 例:
# [    1504] 轨迹=SINE    yaw=  25.98 pitch=   0.00  rate=(  15.7,   0.0) kp=60.0 kd=1.50  TX ok=49 fail=1 【失能】
RE_STATUS = re.compile(
    r"轨迹=(\S+)\s+yaw=\s*(-?\d+\.\d+)\s+pitch=\s*(-?\d+\.\d+)\s+"
    r"rate=\(\s*(-?\d+\.\d+),\s*(-?\d+\.\d+)\)\s+"
    r"kp=(\d+\.\d+)\s+kd=(\d+\.\d+)\s+TX ok=(\d+) fail=(\d+)\s*(.*)"
)

PROFILE_NAMES = {
    "STATIC": "定点", "SINE": "正弦", "SWEEP": "三角扫描",
    "CIRCLE": "圆周", "FIG8": "八字", "MANUAL": "手动",
}

# 记住上次用的串口, 下次双击自动连
CFG_DIR = os.path.join(os.environ.get("APPDATA") or os.path.expanduser("~"), "TargetCar")
CFG_PATH = os.path.join(CFG_DIR, "gui.json")


def cfg_load():
    try:
        with open(CFG_PATH, "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def cfg_save(port):
    try:
        os.makedirs(CFG_DIR, exist_ok=True)
        with open(CFG_PATH, "w", encoding="utf-8") as f:
            json.dump({"port": port}, f)
    except Exception:
        pass


# 主机启动时会打印: [WEB] 手机/电脑连同一个 WiFi, 打开 http://192.168.1.23/
RE_URL = re.compile(r"https?://([0-9]{1,3}(?:\.[0-9]{1,3}){3})")

BG      = "#0a0c11"
CARD    = "#141a23"
CARD2   = "#1b2330"
BORDER  = "#243040"
FG      = "#eef3f9"
DIM     = "#8b98ab"
GREEN   = "#22c55e"
YELLOW  = "#f59e0b"
RED     = "#ef4444"
BLUE    = "#3b82f6"
BTN     = "#212b38"

FONT_SM  = ("Microsoft YaHei UI", 9)
FONT_UI  = ("Microsoft YaHei UI", 10)
FONT_VAL = ("Consolas", 15, "bold")
FONT_TT  = ("Microsoft YaHei UI", 15, "bold")


def _mix(hex_color, other, ratio):
    """把颜色往 other 方向混一点, 用于 hover 高亮。"""
    def rgb(h):
        h = h.lstrip("#")
        return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    a, b = rgb(hex_color), rgb(other)
    return "#%02x%02x%02x" % tuple(int(a[i] + (b[i] - a[i]) * ratio) for i in range(3))


class App:
    def __init__(self, root: tk.Tk, port_hint=None):
        self.root = root
        self.ser = None
        self.reader = None
        self.alive = False
        self.rx_queue = queue.Queue()
        self.status_vars = {}
        self.sliders = {}
        self.host_ip = None

        root.title("靶车控制台 · DM-G6220")
        root.configure(bg=BG)
        root.geometry("1000x640")
        root.minsize(940, 640)

        self._style_ttk()
        self._build_top()
        self._build_scroll_body()
        self._build_status()
        self._build_buttons()
        self._build_sliders()
        self._build_manual()
        self._build_log()

        self.refresh_ports()

        # 串口优先级: 命令行参数 > 上次用的 > 自动挑
        cfg = cfg_load()
        if port_hint:
            self.port_var.set(port_hint)
        elif cfg.get("port") in self.port_box["values"]:
            self.port_var.set(cfg["port"])

        self.root.after(100, self._poll_queue)
        self.root.protocol("WM_DELETE_WINDOW", self.on_close)

        # 上次连过的串口还在的话, 1 秒后自动连上 (双击 bat 就能直接用)
        if self.port_var.get():
            self.root.after(800, self.autoconnect)

    # ---------------- UI ----------------
    def _style_ttk(self):
        st = ttk.Style(self.root)
        try:
            st.theme_use("clam")
        except tk.TclError:
            pass
        st.configure("TCombobox", fieldbackground=CARD2, background=CARD2, foreground=FG,
                     arrowcolor=FG, bordercolor=BORDER, lightcolor=CARD2, darkcolor=CARD2,
                     selectbackground=CARD2, selectforeground=FG, padding=6)
        st.map("TCombobox", fieldbackground=[("readonly", CARD2)],
               foreground=[("readonly", FG)])

    def _build_scroll_body(self):
        outer = tk.Frame(self.root, bg=BG)
        outer.pack(fill="both", expand=True, pady=(6, 0))
        self.canvas = tk.Canvas(outer, bg=BG, highlightthickness=0, bd=0)
        vsb = tk.Scrollbar(outer, orient="vertical", command=self.canvas.yview,
                           width=10, bg=CARD, troughcolor=BG, activebackground=BORDER,
                           relief="flat", bd=0)
        self.canvas.configure(yscrollcommand=vsb.set)
        vsb.pack(side="right", fill="y")
        self.canvas.pack(side="left", fill="both", expand=True)

        self.body = tk.Frame(self.canvas, bg=BG)
        self._body_id = self.canvas.create_window((0, 0), window=self.body, anchor="nw")

        def _on_body(_e=None):
            self.canvas.configure(scrollregion=self.canvas.bbox("all"))
        def _on_canvas(e):
            self.canvas.itemconfigure(self._body_id, width=e.width)
        self.body.bind("<Configure>", _on_body)
        self.canvas.bind("<Configure>", _on_canvas)
        self.canvas.bind_all("<MouseWheel>",
                             lambda e: self.canvas.yview_scroll(int(-e.delta / 120), "units"))
        self.canvas.bind_all("<Button-4>", lambda e: self.canvas.yview_scroll(-1, "units"))
        self.canvas.bind_all("<Button-5>", lambda e: self.canvas.yview_scroll(1, "units"))

    def _card(self, parent, title=None):
        outer = tk.Frame(parent, bg=CARD, highlightbackground=BORDER,
                         highlightthickness=1, bd=0)
        outer.pack(fill="x", padx=12, pady=4)
        if title:
            head = tk.Frame(outer, bg=CARD)
            head.pack(fill="x", padx=12, pady=(8, 0))
            tk.Label(head, text=title, bg=CARD, fg=DIM, font=FONT_SM).pack(side="left")
        inner = tk.Frame(outer, bg=CARD)
        inner.pack(fill="x", padx=10, pady=(6, 10))
        return inner

    def _btn(self, parent, text, cmd, color=BTN, fg=FG, width=8):
        b = tk.Button(parent, text=text, command=cmd, bg=color, fg=fg,
                      activebackground=color, activeforeground=fg, relief="flat", bd=0,
                      width=width, height=1, padx=8, pady=6,
                      font=("Microsoft YaHei UI", 10, "bold"), cursor="hand2",
                      highlightthickness=0)
        b.pack(side="left", padx=4, pady=2)
        b.bind("<Enter>", lambda e: b.configure(bg=_mix(color, "#ffffff", 0.16)))
        b.bind("<Leave>", lambda e: b.configure(bg=color))
        return b

    def _build_top(self):
        head = tk.Frame(self.root, bg=BG)
        head.pack(fill="x", padx=14, pady=(12, 0))
        tk.Label(head, text="靶车控制台", bg=BG, fg=FG, font=FONT_TT).pack(side="left")
        tk.Label(head, text="DM-G6220  ·  ESP32-S3  ·  ESP-NOW", bg=BG, fg=DIM,
                 font=FONT_SM).pack(side="left", padx=12, pady=(5, 0))
        self.link_label = tk.Label(head, text="未连接", bg=CARD2, fg=RED,
                                   font=("Microsoft YaHei UI", 10, "bold"), padx=14, pady=4)
        self.link_label.pack(side="right")

        bar = tk.Frame(self.root, bg=CARD, highlightbackground=BORDER, highlightthickness=1)
        bar.pack(fill="x", padx=12, pady=(10, 4))

        tk.Label(bar, text="串口", bg=CARD, fg=DIM, font=FONT_SM).pack(side="left", padx=(12, 4), pady=8)
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(bar, textvariable=self.port_var, width=16, state="readonly")
        self.port_box.pack(side="left", padx=2, pady=8)

        self.refresh_btn = tk.Button(bar, text="刷新", command=self.refresh_ports, relief="flat",
                                     bg=BTN, fg=FG, activebackground=BTN, activeforeground=FG,
                                     font=FONT_UI, padx=10, pady=4, bd=0, cursor="hand2",
                                     highlightthickness=0)
        self.refresh_btn.pack(side="left", padx=6)

        self.conn_btn = tk.Button(bar, text="连接", command=self.toggle_conn, relief="flat",
                                  bg=BLUE, fg="white", activebackground=BLUE, activeforeground="white",
                                  width=8, font=("Microsoft YaHei UI", 10, "bold"), padx=6, pady=4,
                                  bd=0, cursor="hand2", highlightthickness=0)
        self.conn_btn.pack(side="left", padx=4)

        tk.Label(bar, text=f"{BAUD} 8N1", bg=CARD, fg=DIM, font=FONT_SM).pack(side="left", padx=10)

        self.web_btn = tk.Button(bar, text="打开控制网页", command=self.open_web, relief="flat",
                                 bg=BTN, fg=FG, activebackground=BTN, activeforeground=FG,
                                 font=FONT_UI, padx=10, pady=4, bd=0, cursor="hand2",
                                 highlightthickness=0)
        self.web_btn.pack(side="right", padx=10)
        self.web_btn.bind("<Enter>", lambda e: self.web_btn.configure(bg=_mix(BTN, "#ffffff", 0.16)))
        self.web_btn.bind("<Leave>", lambda e: self.web_btn.configure(bg=BTN))

    def _build_status(self):
        inner = self._card(self.body, "实时状态　·　主机每 500 ms 打印一次，自动解析")
        keys = [("state", "状态"), ("prof", "轨迹"), ("yaw", "目标 yaw"), ("pitch", "目标 pitch"),
                ("rate", "角速度"), ("kp", "kp"), ("kd", "kd"), ("tx", "发包 / 失败")]
        for i, (k, label) in enumerate(keys):
            cell = tk.Frame(inner, bg=CARD2, highlightbackground=BORDER, highlightthickness=1)
            cell.grid(row=i // 4, column=i % 4, sticky="nsew", padx=4, pady=4)
            tk.Label(cell, text=label, bg=CARD2, fg=DIM, font=FONT_SM).pack(anchor="w", padx=10, pady=(7, 0))
            v = tk.StringVar(value="—")
            tk.Label(cell, textvariable=v, bg=CARD2, fg=FG, font=FONT_VAL).pack(anchor="w", padx=10, pady=(1, 7))
            self.status_vars[k] = v
        for c in range(4):
            inner.grid_columnconfigure(c, weight=1, uniform="stat")

    def _build_buttons(self):
        inner = self._card(self.body, "动作")
        self._btn(inner, "使能 E", lambda: self.send("E"), GREEN, "#06240f")
        self._btn(inner, "失能 C", lambda: self.send("C"), "#4b5563")
        self._btn(inner, "急停 X", lambda: self.send("x"), RED, "white", width=10)
        self._btn(inner, "解除 R", lambda: self.send("R"), YELLOW, "#241a02")

        # 轨迹单独一行 (6 个按钮排一排)
        prof = self._card(self.body, "轨迹")
        grid = tk.Frame(prof, bg=CARD)
        grid.pack(fill="x")
        names = ["定点", "正弦", "三角扫描", "圆周", "八字", "手动"]
        self.prof_btns = []
        for i, name in enumerate(names):
            b = tk.Button(grid, text=f"{i} {name}", command=(lambda n=i: self.send(str(n))),
                          bg=BTN, fg=FG, activebackground=BTN, activeforeground=FG,
                          relief="flat", bd=0, cursor="hand2", padx=6, pady=6,
                          font=("Microsoft YaHei UI", 10, "bold"), highlightthickness=0)
            b.grid(row=i // 3, column=i % 3, sticky="ew", padx=4, pady=3)
            grid.grid_columnconfigure(i % 3, weight=1, uniform="prof")
            b.bind("<Enter>", lambda e, w=b: w.configure(bg=_mix(BTN, "#ffffff", 0.16)) if w["bg"] == BTN else None)
            b.bind("<Leave>", lambda e, w=b: w.configure(bg=BTN) if w["bg"] == _mix(BTN, "#ffffff", 0.16) else None)
            self.prof_btns.append(b)

        inner2 = self._card(self.body, "一次性 / 参数")
        self._btn(inner2, "清错误 e", lambda: self.send("e"))
        self._btn(inner2, "存零点 z", self.confirm_zero, YELLOW, "#241a02")
        self._btn(inner2, "保存参数 w", lambda: self.send("w"), BLUE, "white")
        self._btn(inner2, "刷新状态 s", lambda: self.send("s"))
        self._btn(inner2, "重启 b", self.confirm_reboot)

    def _slider_row(self, parent, key, label, frm, to, init, step, cmd, fmt="{:.1f}"):
        row = tk.Frame(parent, bg=CARD)
        row.pack(fill="x", pady=2)
        tk.Label(row, text=label, bg=CARD, fg=DIM, font=FONT_SM, width=18, anchor="w").pack(side="left")
        var = tk.DoubleVar(value=init)
        val_lbl = tk.Label(row, text=fmt.format(init), bg=CARD, fg=BLUE, width=7,
                           font=("Consolas", 12, "bold"))
        val_lbl.pack(side="right")

        def on_change(_=None):
            val_lbl.config(text=fmt.format(var.get()))
        def on_release(_=None):
            v = var.get()
            text = f"{cmd} {v:.1f}" if isinstance(v, float) else f"{cmd} {v}"
            self.send(text)

        sc = tk.Scale(row, from_=frm, to=to, resolution=step, orient="horizontal",
                      variable=var, showvalue=False, bg=CARD, fg=FG, troughcolor="#0d1116",
                      activebackground=BLUE, sliderrelief="flat", sliderlength=22, width=9,
                      highlightthickness=0, bd=0, command=on_change)
        sc.pack(side="left", fill="x", expand=True, padx=8)
        sc.bind("<ButtonRelease-1>", on_release)
        self.sliders[key] = (var, sc, val_lbl, fmt)

    def _build_sliders(self):
        inner = self._card(self.body, "参数（松开滑块才下发；改完 1.5 s 自动存 Flash）")
        self._slider_row(inner, "a", "正弦幅度 a (度)", 0, 180, 10, 1, "a", "{:.0f}")
        self._slider_row(inner, "t", "正弦周期 t (秒)", 1, 30, 6, 1, "t", "{:.0f}")
        self._slider_row(inner, "k", "位置增益 kp", 0, 500, 60, 1, "k", "{:.0f}")
        self._slider_row(inner, "d", "阻尼增益 kd", 0.0, 5.0, 1.5, 0.1, "d", "{:.1f}")

    def _build_manual(self):
        inner = self._card(self.body, "手动角度（自动切到手动模式）")
        tk.Label(inner, text="yaw", bg=CARD, fg=DIM).pack(side="left")
        self.yaw_var = tk.StringVar(value="0")
        e1 = tk.Entry(inner, textvariable=self.yaw_var, width=8, bg="#0d1116", fg=FG,
                      insertbackground=FG, relief="flat", font=("Consolas", 11),
                      highlightthickness=1, highlightbackground=BORDER, highlightcolor=BLUE)
        e1.pack(side="left", padx=4)
        self._btn(inner, "发送 y", lambda: self.send(f"y {self.yaw_var.get()}"))

        tk.Label(inner, text="   pitch", bg=CARD, fg=DIM).pack(side="left")
        self.pitch_var = tk.StringVar(value="0")
        e2 = tk.Entry(inner, textvariable=self.pitch_var, width=8, bg="#0d1116", fg=FG,
                      insertbackground=FG, relief="flat", font=("Consolas", 11),
                      highlightthickness=1, highlightbackground=BORDER, highlightcolor=BLUE)
        e2.pack(side="left", padx=4)
        self._btn(inner, "发送 p", lambda: self.send(f"p {self.pitch_var.get()}"))

    def _build_log(self):
        frame = tk.Frame(self.body, bg=CARD, highlightbackground=BORDER, highlightthickness=1)
        frame.pack(fill="both", expand=True, padx=12, pady=(4, 12))
        bar = tk.Frame(frame, bg=CARD)
        bar.pack(fill="x", padx=12, pady=(8, 4))
        tk.Label(bar, text="串口日志", bg=CARD, fg=DIM, font=FONT_SM).pack(side="left")
        clr = tk.Button(bar, text="清空", command=self.clear_log, relief="flat", bg=BTN, fg=FG,
                        cursor="hand2", font=FONT_SM, padx=10, pady=3, bd=0, highlightthickness=0)
        clr.pack(side="right")
        clr.bind("<Enter>", lambda e: clr.configure(bg=_mix(BTN, "#ffffff", 0.16)))
        clr.bind("<Leave>", lambda e: clr.configure(bg=BTN))

        self.log = tk.Text(frame, bg="#0b0e12", fg="#c8d3df", insertbackground=FG,
                           relief="flat", height=6, wrap="none",
                           font=("Consolas", 9), padx=10, pady=6,
                           highlightthickness=0, bd=0)
        self.log.pack(fill="both", expand=True, padx=10, pady=(0, 10))
        self.log.configure(state="disabled")

    # ---------------- 串口 ----------------
    def refresh_ports(self):
        """只列出"可能是我们板子"的串口: ESP32-S3(303A) / CH340(1A86) / CP210x(10C4)。
        系统自带的 COM3/COM4 (ACPI/蓝牙) 不再出现在下拉框里, 避免选错。"""
        allports = sorted(list_ports.comports(), key=lambda p: p.device)
        want = []
        for p in allports:
            hwid = (p.hwid or "").upper()
            if any(vid in hwid for vid in ("303A", "1A86", "10C4")):
                want.append(p)

        use = want if want else allports
        self.port_box["values"] = [p.device for p in use]

        cur = self.port_var.get()
        if cur and cur in self.port_box["values"]:
            return
        if use:
            self.port_var.set(use[0].device)
        else:
            self.port_var.set("")

    def toggle_conn(self):
        if self.ser:
            self.disconnect()
        else:
            self.connect()

    def connect(self, silent=False):
        port = self.port_var.get()
        if not port:
            if not silent:
                messagebox.showwarning("没有串口", "没找到串口：插上主机板的 USB，再点刷新")
            return
        try:
            self.ser = serial.Serial(port, BAUD, timeout=0.2)
        except Exception as exc:
            if not silent:
                messagebox.showerror("打开失败", f"{port}: {exc}\n\n（别同时开着串口监视器）")
            self.ser = None
            return
        self.alive = True
        self.reader = threading.Thread(target=self._read_loop, daemon=True)
        self.reader.start()
        self.conn_btn.config(text="断开", bg=RED, activebackground=RED)
        self.link_label.config(text=f"{port} 已连接", fg=GREEN)
        self._append(f"== 已连接 {port} @ {BAUD} ==\n")
        cfg_save(port)
        self.root.after(300, lambda: self.send("s"))

    def autoconnect(self):
        """启动时如果上次的串口还在, 直接连上 (双击就能用)。"""
        if not self.ser and self.port_var.get():
            self.connect(silent=True)

    def open_web(self):
        """在浏览器里打开主机的控制页面 (优先用日志里解析到的 IP)。"""
        url = f"http://{self.host_ip}/" if self.host_ip else "http://192.168.4.1/"
        try:
            webbrowser.open(url)
        except Exception:
            pass
        self._append(f"== 打开 {url} ==\n")

    def disconnect(self):
        self.alive = False
        time.sleep(0.05)
        if self.ser:
            try:
                self.ser.close()
            except Exception:
                pass
        self.ser = None
        self.conn_btn.config(text="连接", bg=BLUE, activebackground=BLUE)
        self.link_label.config(text="未连接", fg=RED)
        self._append("== 已断开 ==\n")

    def _read_loop(self):
        buf = b""
        while self.alive and self.ser:
            try:
                data = self.ser.read(self.ser.in_waiting or 1)
            except Exception:
                break
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", "replace").rstrip("\r")
                self.rx_queue.put(text)

    # ---------------- 发送/解析 ----------------
    def send(self, cmd):
        if not self.ser:
            self._append("(未连接) " + cmd + "\n")
            return
        try:
            self.ser.write((cmd + "\r\n").encode("utf-8"))
            self._append(">> " + cmd + "\n")
        except Exception as exc:
            self._append(f"!! 发送失败: {exc}\n")

    def confirm_zero(self):
        if messagebox.askyesno("存零点", "把当前位置保存为 0 点？\n用错会让角度整体偏移！"):
            self.send("z")

    def confirm_reboot(self):
        if messagebox.askyesno("重启主机", "保存参数并重启主机板？\n（页面/串口会断约 2 秒）"):
            self.send("b")

    def _poll_queue(self):
        try:
            while True:
                line = self.rx_queue.get_nowait()
                self._on_line(line)
        except queue.Empty:
            pass
        self.root.after(80, self._poll_queue)

    def _on_line(self, line):
        self._append(line + "\n")

        # 从 [WEB] 日志里抓主机 IP, 供"打开控制网页"用
        mu = RE_URL.search(line)
        if mu:
            self.host_ip = mu.group(1)
            self.link_label.config(text=f"{self.port_var.get()} 已连接 · 页面 {self.host_ip}", fg=GREEN)

        m = RE_STATUS.search(line)
        if not m:
            return
        prof, yaw, pitch, ry, rp, kp, kd, ok, fail, state = m.groups()
        self.status_vars["prof"].set(PROFILE_NAMES.get(prof, prof))
        self.status_vars["yaw"].set(f"{float(yaw):.2f}°")
        self.status_vars["pitch"].set(f"{float(pitch):.2f}°")
        self.status_vars["rate"].set(f"{float(ry):.0f}/{float(rp):.0f}")
        self.status_vars["kp"].set(kp)
        self.status_vars["kd"].set(kd)
        self.status_vars["tx"].set(f"{ok}/{fail}")

        state = (state or "").strip()
        if "急停" in state:
            self.status_vars["state"].set("急停")
        elif "使能" in state:
            self.status_vars["state"].set("使能")
        else:
            self.status_vars["state"].set("失能")

        # 同步滑块位置和数值标签 (比如上电从 Flash 恢复了参数)
        try:
            for _k, _v in (("k", kp), ("d", kd)):
                _var, _sc, _lbl, _fmt = self.sliders[_k]
                _var.set(float(_v))
                _lbl.config(text=_fmt.format(float(_v)))
        except Exception:
            pass

    def _append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text)
        # 只留最后 800 行
        lines = int(self.log.index("end-1c").split(".")[0])
        if lines > 800:
            self.log.delete("1.0", f"{lines-800}.0")
        self.log.see("end")
        self.log.configure(state="disabled")

    def clear_log(self):
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")

    def on_close(self):
        try:
            self.disconnect()
        except Exception:
            pass
        self.root.destroy()


def main():
    # 第一个不是 --flag 的参数当串口名 (例如:  dm_gui.py COM12)
    port = next((a for a in sys.argv[1:] if not a.startswith("--")), None)
    root = tk.Tk()
    app = App(root, port)
    if "--selftest" in sys.argv:
        root.after(400, root.destroy)
    elif "--smoke" in sys.argv:
        # 自动跑几秒, 把日志窗口内容写到文件后退出 (无人值守自检用)
        def _dump():
            try:
                txt = app.log.get("1.0", "end")
                with open("_gui_smoke.txt", "w", encoding="utf-8") as f:
                    f.write(txt)
            except Exception:
                pass
            finally:
                root.destroy()
        root.after(4500, _dump)
    root.mainloop()
    return 0


if __name__ == "__main__":
    sys.exit(main())
