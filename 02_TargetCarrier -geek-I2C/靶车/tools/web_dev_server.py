#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
web_dev_server.py —— 网页控制台的本地开发服务器 (不接板子也能改页面)

用途:
    改 firmware/esp32_master/web/index.html 时, 不用每次都烧固件:
        1) python tools/web_dev_server.py
        2) 浏览器打开 http://127.0.0.1:8000/
        3) 改完 index.html 直接刷新浏览器 (页面每次请求都重新读文件)
        4) 满意后再 cmake --build --preset esp32-master --target flash

它模拟了固件里的三个接口:
    GET /                  -> web/index.html (实时读文件)
    GET /api/status        -> 假状态 JSON (数值会缓慢变化, 方便看动画)
    GET /api/cmd?c=xxx     -> 打印到控制台, 并把参数记进假状态 (kp/kd/轨迹等)
    GET /wifi              -> 提示这是本地模拟

没有第三方依赖 (只用标准库)。
"""

import base64
import hashlib
import json
import os
import random
import struct
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # 靶车/
WEB_DIR = os.path.join(ROOT, "firmware", "esp32_master", "web")
HTML = os.path.join(WEB_DIR, "index.html")
VUE_JS = os.path.join(WEB_DIR, "vue.global.prod.js")

STATE = {
    "online": True, "en": 0, "estop": 0, "mode": "STATIC",
    "yaw": 0.0, "pitch": 0.0, "kp": 60.0, "kd": 1.5, "ok": 0, "fail": 0,
    "heap": 274000, "ch": 6, "up": 0,
}
LOCK = threading.Lock()
T0 = time.time()

MODES = ["STATIC", "SINE", "SWEEP", "CIRCLE", "FIG8", "MANUAL"]

# 预设槽位 (本地模拟; 真板子上存在 NVS 里)
PRESETS = [None, None, None, None]


def ws_frame(payload: bytes) -> bytes:
    """服务端 -> 客户端: 文本帧, 不掩码。"""
    n = len(payload)
    if n < 126:
        hdr = bytes([0x81, n])
    elif n <= 0xFFFF:
        hdr = bytes([0x81, 126]) + struct.pack("!H", n)
    else:
        hdr = bytes([0x81, 127]) + struct.pack("!Q", n)
    return hdr + payload


def log(msg):
    print(msg, flush=True)


def tick():
    """让假数据动起来, 方便看页面刷新是否正常。"""
    while True:
        time.sleep(0.2)
        with LOCK:
            STATE["ok"] += random.randint(4, 6)
            STATE["up"] = int(time.time() - T0)
            mode = STATE["mode"]
            t = time.time() - T0
            if mode == "SINE":
                STATE["yaw"] = 10.0 * __import__("math").sin(2 * 3.14159 * t / 6.0)
            elif mode == "STATIC":
                STATE["yaw"] = 0.0
            else:
                STATE["yaw"] = 5.0 * ((t % 4.0) / 4.0)


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, fmt, *args):  # 静音默认访问日志
        pass

    # ---------------- WebSocket (本地模拟固件的 :81) ----------------
    def handle_ws(self):
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
        log("[ws] 客户端已连接 (20 Hz 推送)")

        try:
            while True:
                with LOCK:
                    data = dict(STATE)
                self.wfile.write(ws_frame(json.dumps(data).encode("utf-8")))
                self.wfile.flush()
                time.sleep(0.05)
        except Exception:
            log("[ws] 客户端断开")

    def _send(self, code, body, ctype="application/json; charset=utf-8"):
        data = body.encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(data)

    def _file(self, path, ctype):
        try:
            with open(path, encoding="utf-8") as f:
                self._send(200, f.read(), ctype)
        except OSError as exc:
            self._send(500, f"读不到 {path}: {exc}", "text/plain; charset=utf-8")

    def do_GET(self):
        u = urlparse(self.path)
        path, q = u.path, parse_qs(u.query)

        if path == "/ws":
            self.handle_ws()
            return

        if path in ("/", "/index.html"):
            self._file(HTML, "text/html; charset=utf-8")
            return

        if path == "/vue.js":
            self._file(VUE_JS, "application/javascript; charset=utf-8")
            return

        if path == "/api/status":
            with LOCK:
                data = dict(STATE)
            self._send(200, json.dumps(data))
            return

        if path == "/api/presets":
            self._send(200, json.dumps([p for p in PRESETS if p]))
            return

        if path.startswith("/api/preset/"):
            act = path.rsplit("/", 1)[-1]
            slot = int((q.get("i") or ["0"])[0])
            name = (q.get("name") or [""])[0]
            if act == "save" and 0 <= slot < 4:
                PRESETS[slot] = {
                    "i": slot, "name": name or f"槽{slot+1}",
                    "kp": STATE["kp"], "kd": STATE["kd"],
                    "a": 10.0, "t": 6.0, "A": 60.0, "v": 20.0, "r": 20.0,
                }
                log(f"[preset] 保存槽 {slot+1} name={name!r}")
            elif act == "load" and 0 <= slot < 4 and PRESETS[slot]:
                p = PRESETS[slot]
                STATE["kp"] = p["kp"]
                STATE["kd"] = p["kd"]
                log(f"[preset] 加载槽 {slot+1}")
            elif act == "del" and 0 <= slot < 4:
                PRESETS[slot] = None
                log(f"[preset] 删除槽 {slot+1}")
            self._send(200, '{"ok":1}')
            return

        if path == "/api/cmd":
            cmd = (q.get("c") or [""])[0].strip()
            log(f"[cmd] {cmd!r}")
            self.apply(cmd)
            self._send(200, '{"ok":1}')
            return

        if path == "/wifi":
            self._send(200, "<meta charset='utf-8'><body style='font-family:sans-serif;padding:24px'>"
                            "本地开发服务器没有配网功能；真板上是 <code>/wifi</code> 表单。</body>",
                       "text/html; charset=utf-8")
            return

        self._send(404, '{"err":"not found"}')

    @staticmethod
    def apply(cmd):
        """把命令映射到假状态, 让界面看起来像真的在动。"""
        if not cmd:
            return
        head, _, arg = cmd.partition(" ")
        with LOCK:
            if head.isdigit():
                STATE["mode"] = MODES[int(head)]
                log(f"      轨迹 -> {STATE['mode']}")
            elif head == "E":
                STATE["en"] = 1
            elif head == "C":
                STATE["en"] = 0
            elif head == "x":
                STATE["estop"] = 1
            elif head == "R":
                STATE["estop"] = 0
            elif head in ("k", "d"):
                try:
                    STATE["kp" if head == "k" else "kd"] = float(arg)
                except ValueError:
                    pass
            elif head in ("y", "p"):
                try:
                    STATE["yaw" if head == "y" else "pitch"] = float(arg)
                except ValueError:
                    pass
                STATE["mode"] = "MANUAL"


def main():
    port = 8000
    if len(sys.argv) > 1:
        port = int(sys.argv[1])
    if not os.path.isfile(HTML):
        sys.stderr.write(f"找不到页面: {HTML}\n")
        return 1

    threading.Thread(target=tick, daemon=True).start()
    srv = ThreadingHTTPServer(("127.0.0.1", port), Handler)
    log(f"网页开发服务器: http://127.0.0.1:{port}/")
    log(f"页面文件      : {HTML}")
    log("改完 index.html 直接刷新浏览器; 命令回显会打印在这里。Ctrl+C 退出。")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        log("\n已退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
