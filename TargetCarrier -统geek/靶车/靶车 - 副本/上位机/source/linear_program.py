"""Editable serial program. Called under the bridge lock; no transport I/O here."""
import copy
import math
import time


def validate_step(value):
    if not isinstance(value, dict):
        raise ValueError('步骤格式无效')
    kind = value.get('kind')
    if kind not in ('x', 'target', 'wait'):
        raise ValueError('请选择 x 轴、靶车或等待模块')
    def number(key, low, high, integer=False):
        n = value.get(key)
        if type(n) not in (int, float) or not math.isfinite(n) or not low <= n <= high or (integer and int(n) != n):
            raise ValueError(f'{key} 范围 {low}–{high}' + ('，请输入整数' if integer else ''))
        return int(n) if integer else float(n)
    result = dict(kind=kind, seconds=number('seconds', .1, 3600))
    if kind == 'x':
        result.update(direction=number('direction', 1, 2, True), speed=number('speed', 1, 100, True),
                      accel=number('accel', 1, 65535, True), decel=number('decel', 1, 65535, True))
    if kind == 'target':
        result.update(yaw_rate=number('yaw_rate', -90, 90), pitch_rate=number('pitch_rate', -60, 60))
        if result['yaw_rate'] == result['pitch_rate'] == 0:
            raise ValueError('靶车速度不能同时为零，请使用等待模块')
    return result


class LinearProgram:
    LIMIT = 200
    TIMEOUT = 5.

    def __init__(self, bridge, lane, clock=time.monotonic):
        self.b, self.clock, self.lane = bridge, clock, lane
        self.rows = []
        self.active = False
        self.phase = 'idle'
        self.message = '添加模块后手动启动；运行中可编辑待执行模块'
        self.current = None
        self.next_id = 1
        self.started = self.deadline = 0
        self.devices = set()
        self.generation = 0

    def edit(self, operation, value=None, ident=None):
        if operation == 'clear':
            if self.active:
                raise ValueError('请先停止程序再清空')
            self.rows.clear()
            self.current = None
            return True
        row = next((r for r in self.rows if r['id'] == ident), None)
        if operation in ('update', 'remove'):
            if row is None or row['state'] != 'pending':
                raise ValueError('只能编辑或删除尚未开始的模块')
            if operation == 'remove':
                self.rows.remove(row)
                return True
        elif operation != 'append':
            raise ValueError('未知程序编辑操作')
        step = validate_step(value)
        if step['kind'] not in (self.lane, 'wait'):
            raise ValueError('动作模块必须加入对应设备的程序线')
        if operation == 'append' and len(self.rows) >= self.LIMIT:
            raise ValueError('最多 200 个模块，请停止后清空历史步骤')
        if self.active and step['kind'] != 'wait' and step['kind'] not in self.devices:
            self._preflight({step['kind']})
            self.devices.add(step['kind'])
            if step['kind'] == 'x':
                self.x_epoch = self.b._session_epoch
            else:
                self.target_generation = self.b._target.snapshot()['generation']
        if operation == 'update':
            row['step'] = step
        else:
            self.rows.append(dict(id=self.next_id, step=step, state='pending'))
            self.next_id += 1
        return True

    def _preflight(self, devices):
        b = self.b
        if 'x' in devices and (not self._x_stopped() or b._state['running']):
            raise ValueError('x 轴需要连接并收到 STM32 停止确认')
        if 'target' in devices:
            t = b._target.snapshot()
            if t['busy'] or not self._target_stopped(t):
                raise ValueError('靶车需新版固件的新鲜 STM32 / 电机失能与零速反馈，旧版状态不能启动联合程序')

    def check_start(self):
        if self.active:
            raise ValueError('程序已经运行')
        if not self.rows:
            raise ValueError('请先添加模块')
        if self.b._closing or self.clock() - self.b._last_focus > .6:
            raise ValueError('请激活窗口再启动')
        target = self.b._target.snapshot()
        if (self.lane == 'x' and self.b._state['running']) or (self.lane == 'target' and
                ((target.get('status') or {}).get('en') or target['busy'])):
            raise ValueError('请先停止手动运动和待发操作，再启动程序')
        devices = {r['step']['kind'] for r in self.rows} - {'wait'}
        self._preflight(devices)
        return devices

    def start(self):
        devices = self.check_start()
        for r in self.rows:
            r['state'] = 'pending'
        self.devices = devices
        self.x_epoch = self.b._session_epoch
        self.target_generation = self.b._target.snapshot()['generation']
        self.active, self.current = True, None
        self.last_tick = self.clock()
        self.phase, self.message = 'next', '程序已启动，按列表顺序执行'
        return True

    def cancel(self, reason='用户停止程序'):
        self.generation += 1
        if self.active:
            if self.current and self.current['state'] == 'current':
                self.current['state'] = 'aborted'
            for row in self.rows:
                if row['state'] == 'pending':
                    row['state'] = 'cancelled'
            self.active = False
            self.phase, self.message = 'aborted', reason + '；剩余动作已作废，需手动重新启动'

    def _x_stopped(self):
        b = self.b
        s = b._state.get('status') or {}
        channel = getattr(b._controller, 'channel', None)
        if channel is not None and (channel.barrier or not channel.synced or s.get('ack') != channel.stop_seq):
            return False
        return bool(b._state['available'] and s.get('online') and s.get('uart') and
                    not s.get('demo') and s.get('flags', 0) & 32 and not s.get('flags', 0) & 64 and
                    s.get('error') == 0 and s.get('motion') == 0 and s.get('phase') == 0 and
                    s.get('current_hz', 0) == 0 and s.get('ack') == s.get('stm_ack'))

    @staticmethod
    def _target_fresh(t):
        s = t.get('status') or {}
        return bool(t['connected'] and s.get('st_fresh') == 1 and s.get('st_online') == 1 and
                    0 <= s.get('st_age', 9999) <= 300 and 0 <= s.get('br_age', 9999) <= 300 and
                    s.get('safe') == 0 and s.get('st_err') == 0)

    def _target_stopped(self, t):
        s = t.get('status') or {}
        return bool(self._target_fresh(t) and s.get('enabled') == 0 and s.get('en') == 0 and
                    s.get('arm') == 0 and s.get('st_state') in (0, 3) and
                    type(s.get('m_vel')) in (int, float) and abs(s['m_vel']) <= .05)

    def _set_phase(self, phase, message):
        self.phase, self.message = phase, message
        self.deadline = self.clock() + self.TIMEOUT

    def tick(self, focused):
        if not self.active:
            return
        try:
            self._tick(focused)
        except Exception as exc:
            self.b._program_fault(str(exc))

    def _tick(self, focused):
        b, now = self.b, self.clock()
        t = b._target.snapshot()
        if not focused or b._closing or now - self.last_tick > .6:
            raise ValueError('窗口失焦或界面心跳中断')
        self.last_tick = now
        if 'x' in self.devices:
            s = b._state.get('status') or {}
            if b._session_epoch != self.x_epoch or not b._state['connected'] or not s.get('online') or s.get('error') != 0:
                raise ValueError('x 轴连接或 STM32 状态异常')
            if self.phase != 'x_stopping' and not b._state['available']:
                raise ValueError('x 轴控制链路未就绪')
        if 'target' in self.devices:
            if not self._target_fresh(t) or t['generation'] != self.target_generation:
                raise ValueError('靶车连接、反馈或控制会话异常')
        if self.phase not in ('next', 'running') and now > self.deadline:
            raise ValueError('启动或停止反馈超过 5 秒，程序中止')
        if self.phase == 'next':
            row = next((r for r in self.rows if r['state'] == 'pending'), None)
            if row is None:
                self.active, self.phase, self.message = False, 'complete', '所有模块执行完成'
                return
            self.current = row
            row['state'] = 'current'
            step = row['step']
            if step['kind'] == 'wait':
                self.started = now
                self._set_phase('running', '等待模块执行中')
            elif step['kind'] == 'x':
                if not self._x_stopped():
                    raise ValueError('x 轴停止尚未确认')
                self.x_revision = b._status_revision
                b._program_move(step)
                self._set_phase('x_starting', '等待 STM32 接受 x 轴动作')
            else:
                if not self._target_stopped(t) or t['busy']:
                    raise ValueError('靶车停止尚未确认')
                if t['locked']:
                    b._target.command('release')
                    self._set_phase('target_release', '解除本程序步骤间的主机急停，等待回报')
                else:
                    self._prepare_target()
        elif self.phase == 'target_release' and not t['busy'] and not t['locked']:
            self._prepare_target()
        elif self.phase == 'target_prepare' and not t['busy']:
            s = t['status']
            if s.get('st_state') != 0 or s.get('cruise') != 0 or s.get('ry') != 0 or s.get('rp') != 0:
                return
            b._target.command('enable')
            self._set_phase('target_enabling', '等待靶车 STM32 授权和电机使能反馈')
        elif self.phase == 'target_enabling' and not t['busy']:
            s = t['status']
            if s.get('enabled') == 1 and s.get('arm') == 1 and s.get('st_state') == 1:
                b._target.command('program_velocity', self.current['step'])
                self._set_phase('target_starting', '等待靶车速度设定回报')
        elif self.phase == 'target_starting' and not t['busy']:
            s, step = t['status'], self.current['step']
            if s.get('enabled') == 1 and s.get('arm') == 1 and s.get('vel') == 1 and s.get('cruise') == 0 and abs(s.get('ry', 1e6) - step['yaw_rate']) <= .051 and abs(s.get('rp', 1e6) - step['pitch_rate']) <= .051:
                self.started = now
                self._set_phase('running', '靶车定时动作执行中（设定时长含运动过程）')
        elif self.phase == 'x_starting':
            s, step = b._state['status'], self.current['step']
            if b._status_revision > self.x_revision and s.get('requested') == step['direction'] and s.get('speed') == step['speed'] and s.get('mode') == 0 and s.get('flags', 0) & 32 and s.get('ack') == s.get('stm_ack'):
                self.started = now
                self._set_phase('running', 'x 轴定时动作执行中（计时含加速，到时减速停止）')
        elif self.phase == 'running':
            step = self.current['step']
            if step['kind'] == 'target' and (t['locked'] or t['status'].get('enabled') != 1 or t['status'].get('arm') != 1):
                raise ValueError('靶车授权或使能反馈丢失')
            if now - self.started >= step['seconds']:
                if step['kind'] == 'wait':
                    self._finish()
                elif step['kind'] == 'x':
                    self.x_revision = b._status_revision
                    b._program_x_stop()
                    self._set_phase('x_stopping', 'x 轴计时结束，等待 STM32 零速停止确认')
                    self.deadline = now + max(self.TIMEOUT, step['speed'] * 10 / step['decel'] + 3)
                else:
                    b._target.stop('程序步骤计时结束')
                    self.target_generation = b._target.snapshot()['generation']
                    self._set_phase('target_stopping', '靶车计时结束，等待失能与零速反馈')
        elif self.phase == 'x_stopping' and b._status_revision > self.x_revision and self._x_stopped():
            self._finish()
        elif self.phase == 'target_stopping' and not t['busy'] and self._target_stopped(t):
            self._finish()

    def _prepare_target(self):
        self.b._target.command('program_prepare')
        self._set_phase('target_prepare', '清除靶车旧轨迹及速度，准备本步骤')

    def _finish(self):
        if self.current['step']['kind'] == 'x':
            # A fast STOP acknowledgement can arrive between owner polls, so
            # an intermediate available=False event is not guaranteed.
            self.b._state['running'] = False
            self.b._run_recipe = None
            self.b._profile_stop_requested = False
        self.current['state'] = 'done'
        self.phase, self.message = 'next', '当前模块完成，准备下一模块'

    def snapshot(self):
        remaining = None
        if self.active and self.phase == 'running':
            remaining = round(max(0, self.current['step']['seconds'] - (self.clock() - self.started)), 1)
        return copy.deepcopy(dict(active=self.active, phase=self.phase, message=self.message,
                                  remaining=remaining, rows=self.rows, generation=self.generation, lane=self.lane))
