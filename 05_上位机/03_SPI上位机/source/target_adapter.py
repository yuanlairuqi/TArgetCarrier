"""Legacy target-car HTTP adapter. Host reports never prove physical STOP.

One network owner, bounded command batches, STOP before recovery, no motion retry.
Only connect() selects an endpoint; construction performs no network activity.
"""
import copy
import json
import math
import threading
import time
from collections import deque
from urllib.parse import urlsplit, quote
from urllib.request import build_opener, ProxyHandler, HTTPRedirectHandler, Request


class NoRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


class HttpClient:
    def __init__(self):
        self.opener = build_opener(ProxyHandler({}), NoRedirect())

    def __call__(self, endpoint, path):
        req = Request(endpoint + path, headers={'Cache-Control': 'no-cache'})
        with self.opener.open(req, timeout=.4) as response:
            body = response.read(16385)
        if len(body) > 16384:
            raise ValueError('主机响应过大')
        return json.loads(body.decode('utf-8'), parse_constant=lambda x: (_ for _ in ()).throw(ValueError(x)))


def endpoint_url(value):
    if not isinstance(value, str) or len(value) > 253 or any(c.isspace() for c in value):
        raise ValueError('请输入主机 IP 或 http://主机:端口')
    p = urlsplit(value if '://' in value else 'http://' + value)
    if p.scheme != 'http' or not p.hostname or p.username or p.password or p.query or p.fragment or p.path not in ('', '/'):
        raise ValueError('仅支持主机 HTTP 地址，不含路径、账户或查询参数')
    if p.port is not None and not 1 <= p.port <= 65535:
        raise ValueError('端口无效')
    return 'http://' + p.netloc


def validate_status(value):
    if not isinstance(value, dict):
        raise ValueError('状态格式无效')
    for name in ('en', 'estop', 'up', 'yaw', 'pitch', 'st_age', 'st_state', 'st_err'):
        v = value.get(name)
        if type(v) not in (float, int) or not math.isfinite(v):
            raise ValueError('缺少有效状态字段: ' + name)
    if value['en'] not in (0, 1) or value['estop'] not in (0, 1) or value['up'] < 0:
        raise ValueError('主机状态值无效')
    # Optional floats are rendered/exported too; JSON NaN is never acceptable.
    if any(type(v) is float and not math.isfinite(v) for v in value.values()):
        raise ValueError('非有限状态值')
    return value


class TargetAdapter:
    POLL = .25
    FRESH = 1.0
    UI_LEASE = .7
    COMMAND_TTL = .6

    def __init__(self, client=None, clock=time.monotonic, background=True):
        self.client = client or HttpClient()
        self.clock = clock
        self.lock = threading.RLock()
        self.wake = threading.Event()
        self.closed = False
        self.endpoint = None
        self.generation = 0
        self.queue = deque(maxlen=8)
        self.logs = deque(maxlen=120)
        self.samples = deque(maxlen=320)
        self.status = None
        self.received = None
        self.last_alive = -1e9
        self.focused = False
        self.connected = False
        self.locked = True
        self.active = False
        self.in_flight = False
        self.stop_pending = False
        self.disconnect_after_stop = False
        self.phase = '未连接'
        self.notice = '填写靶车 ESP32 主机 IP；连接时请求急停和失能'
        self.next_poll = 0
        self.failures = 0
        self.dropped = 0
        self.thread = None
        if background:
            self.thread = threading.Thread(target=self._run, name='target-http-owner', daemon=True)
            self.thread.start()

    def _log(self, kind, message):
        self.logs.append(dict(time=time.strftime('%H:%M:%S'), kind=kind, text=message))

    def _invalidate(self, reason):
        self.generation += 1
        self.dropped += len(self.queue)
        self.queue.clear()
        self.locked = True
        self.active = False
        self.stop_pending = bool(self.endpoint)
        self.next_poll = 0
        self.phase = '停止待确认' if self.endpoint else '未连接'
        self.notice = reason + '；底层停止状态未知'
        self._log('保护', self.notice)

    def connect(self, address):
        endpoint = endpoint_url(address)
        with self.lock:
            if self.endpoint:
                raise ValueError('请先断开当前靶车，再更换地址')
            self.endpoint = endpoint
            self.status = None
            self.received = None
            self.connected = False
            self.samples.clear()
            self._invalidate('连接后先请求主机急停和失能')
            self.next_poll = 0
        self.wake.set()
        return True

    def alive(self, focused):
        with self.lock:
            self.focused = focused is True
            if self.focused:
                self.last_alive = self.clock()
            elif self.active or self.queue:
                self._invalidate('窗口未激活，作废待发动作')
                self.wake.set()

    def stop(self, reason='用户请求停止', disconnect=False):
        with self.lock:
            self._invalidate(reason)
            self.disconnect_after_stop |= disconnect
        self.wake.set()
        return True

    def _fresh(self):
        return self.connected and self.received is not None and self.clock() - self.received <= self.FRESH

    def command(self, name, values=None):
        if name == 'stop':
            return self.stop()
        if name == 'disconnect':
            return self.stop('断开前请求停止', disconnect=True)
        fields = {
            'angles': [('yaw', 'y', -180, 180), ('pitch', 'p', -30, 60)],
            'program_velocity': [('pitch_rate', 'U', -60, 60), ('yaw_rate', 'V', -90, 90)],
            'velocity': [('yaw_rate', 'V', -90, 90), ('pitch_rate', 'U', -60, 60)],
            'gains': [('kp', 'k', 0, 200), ('kd', 'd', 0, 3)],
            'sine': [('amplitude', 'a', 0, 180), ('period', 't', .2, 600)],
            'sweep': [('amplitude', 'A', 0, 180), ('speed', 'v', .1, 180)],
            'cruise': [('speed', 'W', -90, 90)],
            'profile': [('profile', '', 0, 5)],
        }
        fixed = {'program_prepare': ['0', 'U 0', 'V 0'], 'release': ['R'], 'enable': ['E'], 'disable': ['C'], 'clear_error': ['e']}
        if name in fields:
            if not isinstance(values, dict):
                raise ValueError('参数格式无效')
            commands = []
            for key, prefix, lo, hi in fields[name]:
                v = values.get(key)
                if type(v) not in (float, int) or not math.isfinite(v) or not lo <= v <= hi:
                    raise ValueError(f'{key} 范围为 {lo}–{hi}')
                if name == 'profile' and int(v) != v:
                    raise ValueError('模式必须为整数')
                commands.append(prefix + (' ' if prefix else '') + format(v, '.8g'))
        elif name in fixed:
            commands = fixed[name]
        else:
            raise ValueError('不支持的靶车操作')
        with self.lock:
            if not self._fresh() or self.stop_pending or self.disconnect_after_stop:
                raise ValueError('请等待连接和主机停止回报')
            if not self.focused or self.clock() - self.last_alive > self.UI_LEASE:
                raise ValueError('窗口未激活，请重新操作')
            if self.queue or self.in_flight:
                raise ValueError('上一操作处理中，请等待状态刷新')
            if name == 'release':
                if not self.locked or self.status['en'] != 0 or self.status['estop'] != 1:
                    raise ValueError('必须先收到主机失能且急停回报，再手动解除')
            elif name == 'disable':
                return self.stop('用户请求失能')
            elif self.locked:
                raise ValueError('先手动解除主机急停；连接恢复不会自动解锁')
            if name in ('program_prepare', 'program_velocity'):
                if self.status.get('st_fresh') != 1 or self.status.get('safe') != 0:
                    raise ValueError('联合程序需要新版固件的新鲜安全反馈')
                if name == 'program_prepare' and self.status.get('enabled') != 0:
                    raise ValueError('清除旧轨迹前必须确认电机失能')
                if name == 'program_velocity' and (self.status.get('enabled') != 1 or self.status.get('arm') != 1):
                    raise ValueError('定时动作需要电机使能和授权反馈')
            if name == 'enable':
                if self.status['estop'] or self.status['en']:
                    raise ValueError('主机未处于待使能状态')
                if self.status['st_age'] > 600 or self.status['st_state'] not in ((0, 1) if self.status.get('st_fresh') == 1 else (1,)) or self.status['st_err']:
                    raise ValueError('主机转发的 STM32 状态过期或有故障')
                if self.status.get('kp', 0) <= 0 or self.status.get('kd', 0) < .1:
                    raise ValueError('请先设置有效的 kp / kd')
            self.queue.append((self.generation, self.clock() + self.COMMAND_TTL, name, commands))
            if name == 'enable':
                self.active = True
            self._log('操作', name + ' 已排队（尚未执行）')
        self.wake.set()
        return True

    def _send(self, endpoint, command):
        response = self.client(endpoint, '/api/cmd?c=' + quote(command, safe=''))
        if not isinstance(response, dict) or response.get('ok') != 1:
            raise ValueError('主机未接受 HTTP 请求')
        with self.lock:
            self._log('HTTP', command + ' 请求已交付；底层执行未确认')

    def tick(self):
        """One serialized owner iteration; injectable transport supports fault tests."""
        with self.lock:
            endpoint = self.endpoint
            if not endpoint:
                return
            if (self.active or self.queue) and (not self.focused or self.clock() - self.last_alive > self.UI_LEASE):
                self._invalidate('界面心跳失效')
            stop = self.stop_pending
            token = self.generation
            batch = self.queue.popleft() if self.queue and not stop else None
            self.in_flight = bool(batch)
            if not stop and not batch and self.clock() < self.next_poll:
                return
        try:
            if stop:
                # Always try both commands, even if the first request times out.
                errors = []
                for cmd in ('x', 'C'):
                    try:
                        self._send(endpoint, cmd)
                    except Exception as exc:
                        errors.append(str(exc))
                if errors:
                    raise OSError('; '.join(errors))
            elif batch:
                generation, deadline, name, commands = batch
                for cmd in commands:
                    with self.lock:
                        valid = generation == self.generation and self.focused and self.clock() <= deadline and self.clock() - self.last_alive <= self.UI_LEASE
                        if not valid:
                            if generation == self.generation:
                                self._invalidate('排队操作已过期')
                            self.dropped += 1
                            return
                    # STOP can invalidate while I/O is in flight. Never retry motion.
                    self._send(endpoint, cmd)
            result = validate_status(self.client(endpoint, '/api/status'))
            with self.lock:
                if token != self.generation or endpoint != self.endpoint:
                    return
                reboot = self.status is not None and result['up'] < self.status['up']
                recovered = not self.connected and self.received is not None
                self.status = result
                self.received = self.clock()
                self.connected = True
                self.next_poll = self.clock() + self.POLL
                self.samples.append(dict(at=time.time(), **result))
                if reboot or recovered:
                    self._invalidate('主机复位或连接恢复，原操作失效')
                    return
                if stop:
                    self.stop_pending = not (result['en'] == 0 and result['estop'] == 1)
                    self.phase = '主机已锁定' if not self.stop_pending else '停止待确认'
                    self.notice = '主机回报急停 / 失能；STM32 与电机停止仍未独立确认' if not self.stop_pending else '主机尚未回报急停和失能；底层停止未知'
                elif batch and batch[2] == 'release':
                    if result['en'] == 0 and result['estop'] == 0:
                        self.locked = False
                        self.phase = '等待手动使能'
                        self.notice = '仅解除主机急停；需再次手动使能，不自动运动'
                    else:
                        self._invalidate('解除急停后主机状态不符合预期')
                elif not self.locked:
                    self.phase = '主机回报使能' if result['en'] else '等待手动使能'
                    self.notice = '主机命令与转发数据已更新；底层执行和实际运动未独立确认'
                    if result['estop'] or (self.active and (result['st_age'] > 600 or result['st_state'] not in ((0, 1) if result.get('st_fresh') == 1 else (1,)) or result['st_err'])):
                        self._invalidate('主机急停或转发状态异常')
                self.active = self.active or bool(result['en'])
        except Exception as exc:
            with self.lock:
                if token == self.generation:
                    was_connected = self.connected
                    self.connected = False
                    self.failures += 1
                    if was_connected or batch:
                        self.samples.append(dict(at=time.time(), gap=True))
                        self._invalidate('连接或命令失败，旧操作已作废')
                    self.locked = True
                    self.stop_pending = True
                    self.phase = '连接异常 · 停止未知'
                    self.notice = str(exc)[:200] + '；恢复后只请求停止'
                    if self.failures == 1 or was_connected or batch:
                        self._log('错误', self.notice)
                    self.next_poll = self.clock() + .8
        finally:
            with self.lock:
                self.in_flight = False
                if stop and self.disconnect_after_stop:
                    self.endpoint = None
                    self.connected = False
                    self.stop_pending = False
                    self.disconnect_after_stop = False
                    self.phase = '已断开 · 底层停止未知'

    def snapshot(self):
        with self.lock:
            age = None if self.received is None else round((self.clock() - self.received) * 1000)
            fresh = self._fresh()
            return copy.deepcopy(dict(endpoint=self.endpoint, connected=fresh, locked=self.locked,
                busy=bool(self.queue) or self.in_flight or self.stop_pending, phase=self.phase if fresh or not self.connected else '状态过期 · 停止未知',
                notice=self.notice, status=self.status, historical=not fresh, age_ms=age,
                failures=self.failures, dropped=self.dropped, generation=self.generation,
                pending=len(self.queue), physical_stop_confirmed=False, stm_online=None,
                samples=list(self.samples), logs=list(self.logs)))

    def _run(self):
        while not self.closed:
            with self.lock:
                due = self.clock() >= self.next_poll or bool(self.queue)
            if due:
                self.tick()
            self.wake.wait(.04)
            self.wake.clear()

    def close(self):
        self.stop('退出控制台，请求停止', disconnect=True)
        deadline = time.monotonic() + 1.8
        while self.thread and self.endpoint and time.monotonic() < deadline:
            self.wake.set()
            time.sleep(.02)
        self.closed = True
        self.wake.set()
