"""Single asyncio BLE owner. UI requests never call GATT directly."""
import asyncio
import queue
import threading
import time
import logging
from protocol import SERVICE, COMMAND, STATUS, PULSE, Channel, PulseAssembler, parse_status

class Controller:
    def __init__(self, events, client_factory=None, scanner=None):
        self.events = events
        self.client_factory = client_factory
        self.scanner = scanner
        self.loop = asyncio.new_event_loop()
        self.thread = threading.Thread(target=self.loop.run_forever, daemon=True)
        self.thread.start()
        self.client = self.session = self.operation = None
        self.devices = {}
        self.held = None
        self.ui_at = time.monotonic()
        self.ramp=False
        self.soft_stopping=False
        self.channel = Channel()
        self.status = None
        self.closing = False
        self.trace = logging.getLogger('targetcarrier.link')
        self.stats = dict(received=0, offline=0, recovered=0, retries=0, pulse_errors=0)
    def emit(self, kind, value):
        try: self.events.put_nowait((kind, value))
        except queue.Full:
            try: self.events.get_nowait()
            except queue.Empty: pass
            self.events.put_nowait((kind, value))
    def report(self, message):
        logging.getLogger('targetcarrier.link').info(message)
        self.emit('notice', message)
    def call(self, name, *args):
        self.loop.call_soon_threadsafe(self._request, name, args)
    def _request(self, name, args):
        if name == 'alive': self.ui_at = time.monotonic()
        elif name == 'stop': self.stop()
        elif name == 'resync':
            if self.client and self.client.is_connected:
                self.stop(force=True)
                self.report('已请求重新确认停止；需要 ESP 和 STM32 的有效确认后才解锁')
            else: self.report('请先连接 S3，再重新同步 STM32')

        elif name == 'soft_stop':
            if self.available() and self.held and self.ramp:
                a,d=(self.held[5:7] if len(self.held)>=7 else (200,200))
                self.held=(3,0,0,0,0,a,d);self.soft_stopping=True;self.next_send=0
        elif name == 'move':
            if self.available() and not self.soft_stopping:
                self.held = args
                self.next_send = 0
        elif name == 'disconnect':
            if self.session:
                if not self.client or not self.client.is_connected:
                    self.session.cancel()
                else:
                    self.stop()
                    self.closing = time.monotonic() + .8
            elif self.operation: self.operation.cancel()
        elif name in ('scan', 'connect'):
            if self.session or self.operation and not self.operation.done():
                self.emit('busy', bool(self.session or self.operation))
                return
            self.operation = asyncio.create_task(self.scan() if name == 'scan' else self.connect(args[0]))
    def available(self):
        return (self.client is not None and self.client.is_connected and self.status is not None and
                self.status.can_move and self.channel.synced and not self.channel.barrier and
                not self.channel.timed_out and not self.closing)
    def stop(self, force=False):
        self.soft_stopping=False
        self.held = None
        if self.client and self.client.is_connected:
            now = time.monotonic()
            previous = self.channel.stop_seq
            self.channel.stop(now, force=force)
            if self.channel.stop_seq != previous: self.last_stop = now
    def idle_maintenance(self, now):
        """Optional transport maintenance; BLE needs no extra idle traffic."""
        pass
    async def scan(self):
        self.emit('connection', '正在搜索 BLE 设备…')
        try:
            if self.scanner is None:
                from bleak import BleakScanner
                self.scanner = BleakScanner
            found = await self.scanner.discover(timeout=7, return_adv=True)
            self.devices = {key: dev for key, (dev, adv) in found.items()
                            if SERVICE in [u.lower() for u in adv.service_uuids] or
                            (adv.local_name or dev.name or '').startswith('ESP32-Robot')}
            self.emit('devices', [(key, dev.name or 'ESP32-Robot', found[key][1].rssi)
                                  for key, dev in self.devices.items()])
            self.emit('connection', '搜索结束；选择设备连接' if self.devices else '未找到设备：检查蓝牙、电源，并断开手机连接')
        except Exception as e:
            description = str(e)
            if 'POWERED_OFF' in description or 'not powered on' in description:
                description = '电脑蓝牙已关闭，请手动打开 Windows 蓝牙后重新搜索'
            self.emit('connection', '搜索失败：' + description)
        finally: self.emit('busy', False)
    async def connect(self, key):
        self.ramp=getattr(self,'ramp',False);self.soft_stopping=False
        self.channel, self.status, self.pulses = Channel(ramp=self.ramp), None, PulseAssembler()
        self.held, self.closing = None, False
        self.emit('reset', None)
        self.emit('connection', '正在连接 ' + key)
        self.last_status = time.monotonic()
        self.last_stop = self.last_pulse_event = self.last_health = 0
        self.last_health_key = None
        self.stats = dict(received=0, offline=0, recovered=0, retries=0, pulse_errors=0)
        session_token = object()
        self.session_token = session_token
        self.session = asyncio.current_task()
        writer = None
        try:
            if self.client_factory is None:
                from bleak import BleakClient
                self.client_factory = BleakClient
            self.client = self.client_factory(self.devices[key], timeout=12)
            await asyncio.wait_for(self.client.connect(), 15)
            service = self.client.services.get_service(SERVICE)
            if service is None or not service.get_characteristic(COMMAND) or not service.get_characteristic(STATUS):
                raise RuntimeError('设备缺少配套控制服务')
            tx = service.get_characteristic(COMMAND)
            if 'write' not in tx.properties: raise RuntimeError('设备不支持带响应写入')
            # Authenticate/process status first; high volume optional captures
            # must not delay installing the control-status notification handler.
            await asyncio.wait_for(self.client.start_notify(STATUS,
                lambda sender, data: self.receive_status(sender, data) if self.session_token is session_token else None), 5)
            if service.get_characteristic(PULSE):
                try:
                    await asyncio.wait_for(self.client.start_notify(PULSE,
                        lambda sender, data: self.receive_pulse(sender, data) if self.session_token is session_token else None), 5)
                    self.emit('pulse_state', '等待硬件采样')
                except Exception as e:
                    self.emit('pulse_state', '采样订阅失败：' + str(e))
            else: self.emit('pulse_state', '当前固件不支持 PUL/DIR 硬件采集')
            self.last_status = time.monotonic()
            self.next_send = 0
            self.emit('connection', 'BLE 已连接；等待停止确认')
            while self.client.is_connected:
                now = time.monotonic()
                if now - self.last_status > 1.5: raise RuntimeError('状态超过 1.5 秒未更新，已断开')
                if now - self.ui_at > .6 and self.held:
                    self.stop()
                    self.emit('notice', '界面心跳中断，已发送停止')
                if self.held and not self.available(): self.stop()
                self.idle_maintenance(now)
                expired = self.channel.expired(now)
                if expired is not None:
                    self.stop() if expired else None
                    self.report('运动确认超时，已请求停止' if expired else '停止未获 STM32 确认，保留 BLE 连接并等待串口恢复')
                if self.channel.barrier and self.channel.timed_out and now - self.last_stop >= 1.2:
                    # Retry STOP only. Never renew/replay the previous motion.
                    self.stop(force=True)
                    self.stats['retries'] += 1
                    self.report('正在重试停止同步；运动保持锁定')
                if self.closing and (not self.channel.barrier or now >= self.closing): break
                if self.held and now >= self.next_send:
                    self.channel.move(*self.held[:2], now, *self.held[2:])
                    self.next_send = now + .2
                if writer and writer.done():
                    await writer
                    writer = None
                    self.channel.writing = None
                packet = self.channel.take(now)
                if packet:
                    writer = asyncio.create_task(asyncio.wait_for(
                        self.client.write_gatt_char(tx, packet.data, response=True), 1.5))
                    self.emit('tx', packet.data)
                self.emit('available', self.available())
                if now - self.last_health >= 1:
                    self.emit_health(now)
                await asyncio.sleep(.025)
            self.emit('connection', '已断开连接')
        except asyncio.CancelledError:
            self.emit('connection', '连接已取消')
        except Exception as e:
            self.emit('connection', '连接结束：' + str(e))
        finally:
            self.session_token = None
            self.held = None
            if writer:
                writer.cancel()
                await asyncio.gather(writer, return_exceptions=True)
            if self.client:
                try: await asyncio.wait_for(self.client.disconnect(), 3)
                except Exception: pass
            self.client = self.session = None
            self.status = None
            self.emit('available', False)
            self.emit('busy', False)
            self.emit('disconnected', None)
    def emit_health(self, now=None):
        now = time.monotonic() if now is None else now
        self.last_health = now
        self.stats['pulse_errors'] = self.pulses.errors
        health = dict(self.stats, age_ms=round(max(0, now-self.last_status)*1000),
                      stopping=self.channel.barrier, seq=self.channel.stop_seq,
                      online=self.status.online if self.status else False,
                      uart=self.status.uart if self.status else False,
                      flags=self.status.flags if self.status else None,
                      error=self.status.error if self.status else None)
        self.emit('health', health)
    def receive_status(self, sender, data):
        self.emit('raw', bytes(data))
        try: s = parse_status(data)
        except ValueError as e:
            self.stop()
            self.closing = time.monotonic() + .3
            self.emit('notice', str(e) + '，停止并断开')
            return
        previous = self.status
        initial = previous is None
        self.last_status, self.status = time.monotonic(), s
        self.stats['received'] += 1
        if s.uart and not s.online and (initial or not previous.uart or previous.online):
            self.stats['offline'] += 1
            self.report('BLE 仍连接：S3 未收到 STM32 有效串口回包，已锁定运动')
        restored = (not initial and s.uart and s.online and
                    (not previous.uart or not previous.online or not previous.can_move and s.can_move))
        if restored:
            self.stats['recovered'] += 1
            self.report('STM32 串口已恢复；重新确认停止，原运行不会自动续跑')
        needs_stop = initial and s.flags & 1 or restored or (previous and previous.can_move and not s.can_move)
        if needs_stop: self.stop(force=True)

        self.channel.status(s)
        if (self.soft_stopping and s.version==4 and s.requested==3 and s.phase==0 and s.motion==0 and
            s.current_hz==0 and s.ack==s.stm_ack and s.flags&32 and s.error==0 and not self.channel.unconfirmed):
            self.stop() # Fresh hard STOP barrier after the deceleration has completed.
            self.report('减速停止已完成，正在确认停止')
        health_key = (s.flags, s.error, self.channel.barrier)
        if health_key != self.last_health_key:
            logging.getLogger('targetcarrier.link').info('flags=0x%02X error=%s ESP_ACK=%s STM_ACK=%s stop=%s',
                s.flags, s.error, s.ack, s.stm_ack, self.channel.barrier)
            self.last_health_key = health_key
            self.emit_health()
        self.emit('status', s)
    def receive_pulse(self, sender, data):
        capture = self.pulses.receive(data)
        if capture: self.emit('pulse', capture)
        now = time.monotonic()
        if capture or now - self.last_pulse_event >= .2:
            self.last_pulse_event = now
            self.emit('pulse_state', self.pulses.state + '；异常 ' + str(self.pulses.errors))
