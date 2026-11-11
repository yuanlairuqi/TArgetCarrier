#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
tc_app.py —— 靶车控制台 (桌面软件版)

把网页那套 Vue 仪表盘搬成"能双击打开的桌面软件"：
  * 里面跑一个只监听 127.0.0.1 的小服务（HTTP + WebSocket）；
  * 页面还是 web/index.html + vue.global.prod.js（和固件里那份完全同一份）；
  * 数据来自**串口**：自动找 ESP32-S3 的口，460800 读取主机状态行、下发命令；
  * 窗口用 Chrome/Edge 的 --app 模式打开（没有地址栏/标签页，就是一个软件窗口）。

用法:
    pythonw tc_app.py            # 双击 bat / 快捷方式时的入口
    python  tc_app.py --port 8731 --no-window   # 只起服务, 方便调试

依赖: 只用标准库 + pyserial（会和 dm_gui.py 一样去 PlatformIO 的 penv 里找）。
"""

import argparse
import json
import os
import re
import socket
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# ---------------------------------------------------------------------------
# pyserial: 系统 Python 可能没装, 借用 PlatformIO penv 里的
# ---------------------------------------------------------------------------
_penv = os.path.expanduser(os.path.join("~", ".platformio", "penv", "Lib", "site-packages"))
if os.path.isdir(_penv) and _penv not in sys.path:
    sys.path.append(_penv)

try:
    import serial
    from serial.tools import list_ports
except Exception as exc:  # pragma: no cover
    sys.stderr.write(f"需要 pyserial: {exc}\n")
    raise

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WEB = os.path.join(ROOT, "firmware", "esp32_master", "web")
APP_DIR = os.path.join(os.environ.get("APPDATA", os.path.expanduser("~")), "TargetCar")
PRESET_FILE = os.path.join(APP_DIR, "presets.json")
BAUD = 460800

RE_ST = re.compile(r"ST yaw=\s*(-?\d+\.\d+)\s+T=(\d+)C\s+err=(\d+)")

RE_STATUS = re.compile(
    r"轨迹=(\S+)\s+yaw=\s*(-?\d+\.\d+)\s+pitch=\s*(-?\d+\.\d+)\s+"
    r"rate=\(\s*(-?\d+\.\d+),\s*(-?\d+\.\d+)\)\s+"
    r"kp=(\d+\.\d+)\s+kd=(\d+\.\d+)\s+TX ok=(\d+) fail=(\d+)\s*(.*)"
)

STATE = {
    "online": False, "ws": False, "en": 0, "estop": 0, "mode": "—",
    "yaw": 0.0, "pitch": 0.0, "kp": 0.0, "kd": 0.0, "ok": 0, "fail": 0,
    "port": "—", "up": 0, "heap": 0, "ch": 0,
}
LOCK = threading.Lock()
T0 = time.time()
LOG_LINES = []


def log(msg):
    line = time.strftime("%H:%M:%S ") + msg
    with LOCK:
        LOG_LINES.append(line)
        del LOG_LINES[:-400]
    print(line, flush=True)


# ---------------------------------------------------------------------------
# 串口
# ---------------------------------------------------------------------------
class Link:
    def __init__(self):
        self.ser = None
        self.port = None
        self.last_rx = 0.0
        self.lock = threading.Lock()

    @staticmethod
    def pick_port():
        cands = []
        for p in list_ports.comports():
            hwid = (p.hwid or "").upper()
            if "303A" in hwid:            # ESP32-S3 原生 USB
                cands.append(p.device)
        if cands:
            return sorted(cands)[0]
        for p in list_ports.comports():
            if "1A86" in (p.hwid or "").upper():   # CH340
                cands.append(p.device)
        return sorted(cands)[0] if cands else None

    @staticmethod
    def candidates():
        """所有可能是 ESP32 的串口, 303A 排前面"""
        out = []
        for p in list_ports.comports():
            hwid = (p.hwid or "").upper()
            # 只考虑 USB 串口, 跳过蓝牙虚拟串口 (打开会卡十几秒)
            if not any(k in hwid for k in ("USB", "303A", "1A86", "10C4", "0403")):
                continue
            score = 0 if "303A" in hwid else (1 if "1A86" in hwid else 2)
            out.append((score, p.device))
        out.sort()
        return [dev for _, dev in out]

    def _probe(self, port, seconds=1.4):
        """打开端口看看有没有主机状态行; 有就留着, 没有就关掉"""
        try:
            ser = serial.Serial(port, BAUD, timeout=0.2)
        except Exception:
            return None
        ser.setDTR(False)
        ser.setRTS(False)
        deadline = time.time() + seconds
        buf = b""
        try:
            while time.time() < deadline:
                chunk = ser.read(256)
                if chunk:
                    buf += chunk
                    if RE_STATUS.search(buf.decode("utf-8", "replace")):
                        return ser
        except Exception:
            pass
        try:
            ser.close()
        except Exception:
            pass
        return None

    def open(self):
        for port in self.candidates():
            ser = self._probe(port)
            if not ser:
                continue
            with self.lock:
                self.ser = ser
                self.port = port
            with LOCK:
                STATE["port"] = port
            log(f"[串口] 已连接 {port} @ {BAUD} (有主机状态行)")
            threading.Thread(target=self._reader, args=(ser,), daemon=True).start()
            return True
        return False

        ser.setDTR(False)
        ser.setRTS(False)
        with self.lock:
            self.ser = ser
            self.port = port
        with LOCK:
            STATE["port"] = port
        log(f"[串口] 已连接 {port} @ {BAUD}")
        threading.Thread(target=self._reader, args=(ser,), daemon=True).start()
        return True

    def _reader(self, ser):
        buf = b""
        while True:
            try:
                chunk = ser.read(256)
            except Exception:
                break
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                self._on_line(line.decode("utf-8", "replace").strip())

        log("[串口] 断开")
        with self.lock:
            self.ser = None
        with LOCK:
            STATE["online"] = False

    def _on_line(self, line):
        if not line:
            return
        m = RE_STATUS.search(line)
        if not m:
            return
        self.last_rx = time.time()
        mode, yaw, pitch, ry, rp, kp, kd, ok, fail, tail = m.groups()
        st = RE_ST.search(line)
        with LOCK:
            if st:
                STATE["st_yaw"] = float(st.group(1))
                STATE["st_temp"] = int(st.group(2))
                STATE["st_err"] = int(st.group(3))
            STATE.update({
                "online": True, "mode": mode,
                "yaw": float(yaw), "pitch": float(pitch),
                "kp": float(kp), "kd": float(kd),
                "ry": float(ry), "rp": float(rp),
                "ok": int(ok), "fail": int(fail),
                "estop": 1 if "急停" in tail else 0,
                "en": 1 if ("使能" in tail and "失能" not in tail and "急停" not in tail) else 0,
                "up": int(time.time() - T0),
            })

    def send(self, cmd):
        with self.lock:
            ser = self.ser
        if not ser:
            log(f"[命令] (未连接) {cmd}")
            return False
        try:
            ser.write((cmd + "\r\n").encode("utf-8"))
            log(f"[命令] {cmd}")
            return True
        except Exception as exc:
            log(f"[命令] 发送失败 {cmd}: {exc}")
            return False


LINK = Link()


# ---------------------------------------------------------------------------
# 预设 (本地文件, 4 个槽)
# ---------------------------------------------------------------------------
def load_presets():
    try:
        with open(PRESET_FILE, encoding="utf-8") as f:
            data = json.load(f)
        if isinstance(data, list) and len(data) == 4:
            return data
    except Exception:
        pass
    return [None, None, None, None]


def save_presets(data):
    os.makedirs(APP_DIR, exist_ok=True)
    with open(PRESET_FILE, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=2)


# ---------------------------------------------------------------------------
# HTTP + WebSocket
# ---------------------------------------------------------------------------
def ws_frame(payload: bytes) -> bytes:
    n = len(payload)
    if n < 126:
        return bytes([0x81, n]) + payload
    if n <= 0xFFFF:
        return bytes([0x81, 126]) + n.to_bytes(2, "big") + payload
    return bytes([0x81, 127]) + n.to_bytes(8, "big") + payload


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    last_seen = time.time()

    def log_message(self, fmt, *args):
        pass

    # ---------------- 基础 ----------------
    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        if isinstance(body, str):
            body = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def _file(self, path, ctype):
        try:
            with open(path, "rb") as f:
                self._send(200, f.read(), ctype)
        except OSError as exc:
            self._send(500, f"读不到 {path}: {exc}", "text/plain; charset=utf-8")

    # ---------------- GET ----------------
    def do_GET(self):
        Handler.last_seen = time.time()
        u = urlparse(self.path)
        path, q = u.path, parse_qs(u.query)

        if path in ("/", "/index.html"):
            self._file(os.path.join(WEB, "index.html"), "text/html; charset=utf-8")
            return

        if path == "/vue.js":
            self._file(os.path.join(WEB, "vue.global.prod.js"), "application/javascript; charset=utf-8")
            return

        if path == "/api/status":
            with LOCK:
                data = dict(STATE)
            # 串口在、且 2 秒内收到过状态行, 才算在线
            data["online"] = bool(LINK.ser) and (time.time() - LINK.last_rx < 2.0)
            self._send(200, json.dumps(data))
            return

        if path == "/api/cmd":
            cmd = (q.get("c") or [""])[0].strip()
            ok = LINK.send(cmd) if cmd else False
            self._send(200, json.dumps({"ok": 1 if ok else 0}))
            return

        if path == "/api/presets":
            ps = load_presets()
            out = [{"i": i, **(p or {})} for i, p in enumerate(ps) if p]
            self._send(200, json.dumps(out, ensure_ascii=False))
            return

        if path.startswith("/api/preset/"):
            self._preset(path.rsplit("/", 1)[-1], q)
            return

        if path == "/ws":
            self._ws()
            return

        if path == "/wifi":
            self._send(200, "<meta charset='utf-8'><body style='font-family:sans-serif;padding:24px'>"
                            "桌面版不需要配网；配网请在手机浏览器打开主机的 <code>/wifi</code>。</body>",
                       "text/html; charset=utf-8")
            return

        self._send(404, '{"err":"not found"}')

    # ---------------- 预设 ----------------
    def _preset(self, act, q):
        slot = int((q.get("i") or ["0"])[0])
        if not (0 <= slot < 4):
            self._send(400, '{"ok":0}')
            return
        name = (q.get("name") or [""])[0][:15]
        ps = load_presets()
        with LOCK:
            kp, kd = STATE["kp"], STATE["kd"]

        if act == "save":
            ps[slot] = {"i": slot, "name": name or f"槽{slot+1}", "kp": kp, "kd": kd,
                        "a": 10, "t": 6, "A": 60, "v": 20, "r": 20}
            save_presets(ps)
            log(f"[预设] 保存槽 {slot+1}: {ps[slot]['name']} kp={kp:.1f} kd={kd:.2f}")
        elif act == "load" and ps[slot]:
            p = ps[slot]
            for cmd in (f"k {p['kp']:.1f}", f"d {p['kd']:.2f}"):
                LINK.send(cmd)
                time.sleep(0.05)
            log(f"[预设] 加载槽 {slot+1}")
        elif act == "del":
            ps[slot] = None
            save_presets(ps)
            log(f"[预设] 删除槽 {slot+1}")
        self._send(200, '{"ok":1}')

    # ---------------- WebSocket ----------------
    def _ws(self):
        import base64
        import hashlib
        key = self.headers.get("Sec-WebSocket-Key", "")
        if not key:
            self._send(400, '{"err":"no key"}')
            return
        acc = base64.b64encode(hashlib.sha1(
            (key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11").encode()
        ).digest()).decode()
        self.send_response(101, "Switching Protocols")
        self.send_header("Upgrade", "websocket")
        self.send_header("Connection", "Upgrade")
        self.send_header("Sec-WebSocket-Accept", acc)
        self.end_headers()
        try:
            while True:
                with LOCK:
                    data = dict(STATE)
                self.wfile.write(ws_frame(json.dumps(data).encode("utf-8")))
                self.wfile.flush()
                time.sleep(0.05)
        except Exception:
            pass


# ---------------------------------------------------------------------------
# 启动
# ---------------------------------------------------------------------------
def free_port(preferred):
    for p in [preferred] + list(range(preferred + 1, preferred + 20)):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            s.close()
            return p
        except OSError:
            s.close()
    return preferred


def find_browser():
    cands = [
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Microsoft\Edge\Application\msedge.exe",
    ]
    for c in cands:
        if os.path.exists(c):
            return c
    return None


def open_window(url):
    """优先用 pywebview (真·原生窗口), 没有再退回 Chrome/Edge 的 --app 模式。"""
    try:
        import webview  # type: ignore
        threading.Thread(target=lambda: webview.create_window("靶车控制台", url, width=1280, height=860),
                         daemon=True).start()
        webview.start()
        return
    except Exception:
        pass

    browser = find_browser()
    if not browser:
        log("[窗口] 没找到 Chrome/Edge, 请手动打开: " + url)
        return
    import subprocess
    profile = os.path.join(APP_DIR, "browser_profile")
    subprocess.Popen([browser, f"--app={url}", f"--user-data-dir={profile}",
                      "--window-size=1280,880", "--no-first-run"],
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    log("[窗口] 已用浏览器 app 模式打开")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8731)
    ap.add_argument("--no-window", action="store_true")
    ap.add_argument("--port-name", default=None, help="指定串口, 例如 COM12")
    args = ap.parse_args()

    os.makedirs(APP_DIR, exist_ok=True)

    port = free_port(args.port)
    url = f"http://127.0.0.1:{port}/"
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    log(f"[服务] {url}")

    # 串口放后台连: 蓝牙串口打开可能卡十几秒, 不能挡住页面
    def serial_manager():
        while True:
            if LINK.ser is None:
                try:
                    if args.port_name:
                        ser = LINK._probe(args.port_name, 1.0)
                        if ser:
                            with LINK.lock:
                                LINK.ser = ser
                                LINK.port = args.port_name
                            with LOCK:
                                STATE["port"] = args.port_name
                            log(f"[串口] 已连接 {args.port_name}")
                            threading.Thread(target=LINK._reader, args=(ser,), daemon=True).start()
                    else:
                        LINK.open()
                except Exception as exc:
                    log(f"[串口] 连接异常: {exc}")
            time.sleep(1.0)

    threading.Thread(target=serial_manager, daemon=True).start()

    if not args.no_window:
        open_window(url)

    started = time.time()
    try:
        while True:
            time.sleep(0.5)
            # 窗口关了 (8 秒没有任何请求) 就退出, 不在后台留进程
            if (not args.no_window) and (time.time() - started > 15) \
                    and (time.time() - Handler.last_seen > 8):
                log("窗口已关闭, 退出")
                break
    except KeyboardInterrupt:
        log("退出")


if __name__ == "__main__":
    main()
