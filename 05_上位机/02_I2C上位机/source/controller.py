"""Single asyncio BLE owner. UI requests never call GATT directly."""
import asyncio
import os
import queue
import threading
import time
import logging
from types import SimpleNamespace
from ble_connect import open_ble, discover_devices, device_matches, connection_error
from protocol import SERVICE, COMMAND, STATUS, PULSE, Channel, PulseAssembler, parse_status
from bounded_gatt import WriteChannelTimeout,close_session,cleanup_pending,bounded_await
from x_diagnostics import DIAGNOSTIC,parse_diagnostic
from diag_timeline import Timeline
from ext_diag import EXT1, EXT2, EXT_UUIDS

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
        self.motion_generation=0
        self.ramp=False
        self.soft_stopping=False
        self.channel = Channel()
        self.status = None
        self.closing = False
        self.trace = logging.getLogger('targetcarrier.link')
        # Diagnostic timeline (see diag_timeline.py). Evidence only: never alters
        # control flow and can be disabled with TC_DIAG_TIMELINE=0.
        self.timeline = Timeline()
        self.ext = {}
        self.ext_read_timeout = 1.0
        self._ext_task = None
        self._write_meta = None
        self.stats = dict(received=0, offline=0, recovered=0, retries=0, pulse_errors=0)
    def _diag(self):
        """Lazily create the diagnostic timeline (also for partial test owners)."""
        timeline = getattr(self, 'timeline', None)
        if timeline is None:
            timeline = self.timeline = Timeline()
        return timeline

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
        if name=='move':
            generation=getattr(self,'motion_generation',0);created=time.monotonic()
            self.loop.call_soon_threadsafe(lambda:self._request(name,args) if generation==getattr(self,'motion_generation',0) and time.monotonic()-created<.1 else None)
        else:self.loop.call_soon_threadsafe(self._request,name,args)
    def _request(self, name, args):
        if name == 'alive': self.ui_at = time.monotonic()
        elif name == 'stop': self.stop()
        elif name == 'resync':
            if self.client and self.client.is_connected:
                self.stop(force=True)
                self.report('已请求重新确认停止；需要 ESP 和 STM32 的有效确认后才解锁')
            else: self.report('请先连接 S3，再重新同步 STM32')

        elif name == 'limit_reset':
            if not self.limit_connected() or time.monotonic() - self.ui_at > .6:
                self.report('限位复位未发送：需要通信在线、窗口激活及 STM32 停止确认')
                return
            self.motion_generation = getattr(self, 'motion_generation', 0) + 1
            self.held = None
            self.soft_stopping = False
            self.limit_reset_seq = self.channel.reset_limit(time.monotonic())
            self.last_stop = time.monotonic()
            self.report('限位复位请求已发送；等待从机检查输入及 STM32 停止反馈')

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
                not self.channel.timed_out and not self.closing and
                (not hasattr(self.client,"transport_ready") or self.client.transport_ready()))
    def limit_connected(self):
        return bool(self.client and self.client.is_connected and self.status and self.status.error == 10 and
            self.status.stop_feedback and self.channel.synced and not self.channel.barrier and
            not self.channel.timed_out and not self.closing and
            (not hasattr(self.client, 'transport_ready') or self.client.transport_ready()))

    def stop(self, force=False):
        self.motion_generation=getattr(self,"motion_generation",0)+1
        self.limit_reset_seq=None
        self.soft_stopping=False
        self.held = None
        if self.client and self.client.is_connected:
            now = time.monotonic()
            previous = self.channel.stop_seq
            self.channel.stop(now, force=force)
            if self.channel.stop_seq != previous:
                self.last_stop = now
                logging.getLogger('targetcarrier.link').info('X STOP queued seq=%s force=%s',self.channel.stop_seq,force)
                try:
                    self._diag().stop_queued(self.channel.stop_seq, force,
                                              blocked_by_write=self.channel.writing is not None, now=now)
                except Exception:pass
    def idle_maintenance(self, now):
        """Retry STOP only; a confirmed stop never clears a motion fault itself."""
        channel=self.channel;status=self.status
        if (not self.client or not self.client.is_connected or self.held is not None
            or now-getattr(self,'last_stop',0)<1.2 or now-getattr(self,'last_status',0)>.6
            or channel.pending is not None or channel.writing is not None or channel.unconfirmed):return
        if channel.barrier:
            self.stop(force=True)
            if hasattr(self,'stats'):self.stats['retries'] += 1
            self.report('故障后重新确认停止；只发送 STOP，故障清除前禁止启动')
    async def scan(self):
        self.devices={}
        self.last_scan_error=None
        self.emit('devices',[])
        self.emit('connection', '正在搜索 BLE 设备…')
        try:
            if self.scanner is None:
                from bleak import BleakScanner
                self.scanner = BleakScanner
            found = await discover_devices(self.scanner, timeout=7, return_adv=True)
            self.devices = {key: dev for key, (dev, adv) in found.items()
                            if device_matches(dev, adv, SERVICE, ('TC-XAxis','ESP32-Robot'))}
            self.emit('devices', [(key, dev.name or 'ESP32-Robot', found[key][1].rssi)
                                  for key, dev in self.devices.items()])
            self.emit('connection', '搜索结束；选择设备连接' if self.devices else '未找到设备：检查蓝牙、电源，并断开手机连接')
            return True
        except Exception as e:
            description = str(e)
            if 'POWERED_OFF' in description or 'not powered on' in description:
                description = '电脑蓝牙已关闭，请手动打开 Windows 蓝牙后重新搜索'
            self.last_scan_error=description
            self.emit('connection', '搜索失败：' + description)
            return False
        finally: self.emit('busy', False)
    async def connect(self, key):
        if cleanup_pending(self):
            if not key.startswith('usb:'):self._reconnect_enabled=False
            self.emit('busy',False)
            self.report('旧 GATT 操作尚未释放，禁止建立新控制会话；请等待或重启上位机')
            return
        self.motion_generation=getattr(self,"motion_generation",0)+1
        self.ramp=getattr(self,'ramp',False);self.soft_stopping=False
        self.channel, self.status, self.pulses = Channel(ramp=self.ramp), None, PulseAssembler()
        self.held, self.closing = None, False
        self.limit_reset_seq = None
        self.emit('reset', None)
        self.emit('connection', '正在连接 ' + key)
        self.last_status = time.monotonic()
        self.last_stop = self.last_pulse_event = self.last_health = 0
        self.last_health_key = None
        self.stats = dict(received=0, offline=0, recovered=0, retries=0, pulse_errors=0)
        self._diag().reset_session(self.last_status)
        self.ext = {}
        self._ext_task = None
        self._write_meta = None
        session_token = object()
        self.session_token = session_token
        self.session = asyncio.current_task()
        writer = None
        writer_started = 0
        try:
            if self.client_factory is None:
                from bleak import BleakClient
                self.client_factory = BleakClient
            if key.startswith('usb:'):
                self.client = self.client_factory(self.devices[key], timeout=12)
                if hasattr(self.client,'on_link_loss'):self.client.on_link_loss=self.transport_loss
                await bounded_await(self,self.client.connect(),15)
            else:
                self.client = await open_ble(self.client_factory,self.devices[key],SERVICE,lambda message:self.emit('connection',message),owner=self)
            service = self.client.services.get_service(SERVICE)
            if service is None or not service.get_characteristic(COMMAND) or not service.get_characteristic(STATUS):
                raise RuntimeError('设备缺少配套控制服务')
            tx = service.get_characteristic(COMMAND)
            if 'write' not in tx.properties: raise RuntimeError('设备不支持带响应写入')
            # Authenticate/process status first; high volume optional captures
            # must not delay installing the control-status notification handler.
            self.emit('connection','BLE / USB 服务已确认；正在订阅状态通知')
            await bounded_await(self,self.client.start_notify(STATUS,
                lambda sender, data: self.receive_status(sender, data) if self.session_token is session_token else None), 5)
            if service.get_characteristic(DIAGNOSTIC):
                try:
                    await bounded_await(self,self.client.start_notify(DIAGNOSTIC,
                        lambda sender,data:self.receive_diagnostic(data) if self.session_token is session_token else None),3)
                except WriteChannelTimeout:raise
                except Exception as e:logging.getLogger('targetcarrier.link').warning('Optional ESP diagnostics unavailable: %s',e)
            pulse_subscribe = os.environ.get('TC_PULSE_SUBSCRIBE', 'on').strip().lower() not in ('0','off','false','no','disabled')
            if service.get_characteristic(PULSE):
                if not pulse_subscribe:
                    # Diagnostic A/B switch only: default (unset/on) keeps the
                    # existing unconditional subscription behaviour.
                    self.emit('pulse_state', '诊断开关 TC_PULSE_SUBSCRIBE=off：未订阅 PULSE（仅控制/状态对照）')
                    logging.getLogger('targetcarrier.diag').info('PULSE subscribe skipped by TC_PULSE_SUBSCRIBE=off')
                else:
                    try:
                        await bounded_await(self,self.client.start_notify(PULSE,
                            lambda sender, data: self.receive_pulse(sender, data) if self.session_token is session_token else None), 5)
                        self.emit('pulse_state', '等待硬件采样')
                    except WriteChannelTimeout:raise
                    except Exception as e:
                        self.emit('pulse_state', '采样订阅失败：' + str(e))
            else: self.emit('pulse_state', '当前固件不支持 PUL/DIR 硬件采集')
            # Optional read-only ESP diagnostic characteristics (see ext_diag.py).
            # Firmware without them is normal: log once, keep the control path as is.
            if not key.startswith('usb:'):
                for name, uuid in EXT_UUIDS:
                    try: self.ext[name] = service.get_characteristic(uuid)
                    except Exception: self.ext[name] = None
                missing = [name for name, _uuid in EXT_UUIDS if not self.ext.get(name)]
                if self._diag().ext_enabled and missing:
                    self._diag().ext_unavailable(missing)
                # EXT reads are read-only and use a throwaway owner: a hung read
                # must never fence a new control session (quarantine applies to
                # writes/clients only).
                self._ext_owner = SimpleNamespace()
            self.last_status = time.monotonic()
            self.next_send = 0
            self.emit('ble_ready',not key.startswith('usb:'))
            self.emit('busy',False)
            self.emit('connection', ('USB 已连接' if hasattr(self.client,'transport_ready') else 'BLE 已连接')+'；等待停止确认')
            while self.client.is_connected:
                now = time.monotonic()
                if writer is not None and not writer.done() and now-writer_started>=getattr(self,'write_deadline',1.5):
                    # Never clear the slot and concurrently inject STOP behind a late motion write.
                    meta=getattr(self,'_write_meta',None) or (None,None,writer_started)
                    self._diag().write_deadline(meta[0], round((now-writer_started)*1000),
                                                 round(getattr(self,'write_deadline',1.5)*1000))
                    self.session_token=None;self.held=None
                    self.motion_generation=getattr(self,'motion_generation',0)+1
                    self.channel.synced=False;self.channel.barrier=True;self.channel.timed_out=True
                    self.channel.pending=None;self.channel.unconfirmed.clear()
                    if not key.startswith('usb:'):self._reconnect_enabled=False
                    self.emit('available',False)
                    self.report('BLE/USB 写通道超时；停止未确认，正在释放旧会话；恢复后需手动连接并确认 STOP')
                    self._diag().quarantine('write_deadline')
                    raise WriteChannelTimeout('write deadline exceeded; old operation quarantined')
                if now-self.last_status>1.5 and not hasattr(self.client,'transport_ready'): raise RuntimeError('状态超过 1.5 秒未更新，已断开')
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
                    # Same await/ordering as before; only the diagnostic line is new.
                    meta=getattr(self,'_write_meta',None) or (None,None,writer_started)
                    seq, motion, started_at = meta
                    try:
                        await writer
                    except asyncio.CancelledError:
                        self._diag().write_done(seq, (time.monotonic()-started_at)*1000, ok=False,
                                                 kind='CancelledError', motion=motion)
                        raise
                    except Exception as exc:
                        self._diag().write_done(seq, (time.monotonic()-started_at)*1000, ok=False,
                                                 kind=type(exc).__name__, motion=motion)
                        self._diag().write_native(seq, 'error:'+type(exc).__name__)
                        raise
                    self._diag().write_done(seq, (time.monotonic()-started_at)*1000, ok=True, motion=motion)
                    writer = None
                    self.channel.writing = None
                    self._write_meta = None
                packet = self.channel.take(now)
                if packet:
                    logging.getLogger('targetcarrier.link').info('X TX seq=%s op=%s speed=%s stop_seq=%s',packet.seq,packet.motion,packet.data[2],self.channel.stop_seq)
                    self._diag().write_submit(packet.seq, packet.motion, self.channel.stop_seq, now)
                    writer_started=time.monotonic()
                    writer = asyncio.create_task(self.client.write_gatt_char(tx, packet.data, response=True))
                    self._write_meta = (packet.seq, packet.motion, writer_started)
                    self.emit('tx', packet.data)
                self.emit('available', self.available())
                self._ext_tick(now)
                if now - self.last_health >= 1:
                    self.emit_health(now)
                    self._diag().second(now, self.status, self.channel)
                await asyncio.sleep(.025)
            self.emit('connection', '已断开连接')
        except asyncio.CancelledError:
            self.emit('connection', '连接已取消')
        except Exception as e:
            if not key.startswith('usb:') and (writer is not None or isinstance(e,WriteChannelTimeout)):
                self._reconnect_enabled=False
            logging.getLogger('targetcarrier.link').exception('X-axis connection failed: %r',e)
            self.emit('connection', '连接结束：' + connection_error(e))
        finally:
            self.session_token = None
            self.held = None
            self.motion_generation=getattr(self,'motion_generation',0)+1
            self.channel.synced=False;self.channel.barrier=True
            self.channel.pending=None;self.channel.unconfirmed.clear()
            ext_task=self._ext_task
            self._ext_task=None
            if ext_task is not None and not ext_task.done():
                ext_task.cancel()
                try: await asyncio.wait([ext_task],timeout=.2)
                except Exception:pass
            if writer is not None and not writer.done():
                # Diagnostic only: record what the native write finally reports
                # (done/cancelled/error) even after the session is gone.
                meta=getattr(self,'_write_meta',None) or (None,None,0)
                self._watch_native(meta[0],writer)
            released=True
            try:
                released=await close_session(self,self.client,writer,getattr(self,'cleanup_deadline',3.0))
                if not released:self.report('旧 GATT 清理未完成；运动保持锁定，禁止重连运动；请等待或重启上位机')
            except asyncio.CancelledError:pass
            try:self._diag().close('session_released' if released else 'session_quarantined')
            except Exception:pass
            self.client = self.session = None
            self.status = None
            self.emit('available', False)
            self.emit('busy', False)
            self.emit('disconnected', None)
    def _watch_native(self, seq, task):
        """Log the eventual native completion state of a quarantined write."""
        try:
            def finished(t):
                try:
                    if t.cancelled(): state='cancelled'
                    elif t.exception() is not None: state='error:'+type(t.exception()).__name__
                    else: state='done'
                    self._diag().write_native(seq, state)
                except Exception: pass
            task.add_done_callback(finished)
        except Exception: pass
    def transport_loss(self,reason):
        self.motion_generation=getattr(self,"motion_generation",0)+1
        self.stop(force=True);self.channel.synced=False;self.status=None
        self.emit('available',False);self.emit('transport_lost',reason)
        self.report(reason+'；未确认已停止，恢复后需用户重新启动')
    def emit_health(self, now=None):
        now = time.monotonic() if now is None else now
        self.last_health = now
        self.stats['pulse_errors'] = self.pulses.errors
        health = dict(self.stats, age_ms=round(max(0, now-self.last_status)*1000),
                      stopping=self.channel.barrier, seq=self.channel.stop_seq,
                      online=self.status.online if self.status else False,
                      uart=self.status.uart if self.status else False,
                      flags=self.status.flags if self.status else None,
                      error=self.status.error if self.status else None, limit_latched=bool(self.status and self.status.error == 10),
                      stop_confirmed=bool(self.status and self.status.stop_feedback and self.channel.synced and not self.channel.barrier),
                      ack_delay_ms=getattr(self.channel,'ack_delay_ms',None),
                      recovery_state=('awaiting_stop' if self.channel.barrier else 'fault_latched' if self.status and self.status.error else 'ready' if self.available() else 'not_ready'))
        self.emit('health', health)
    def _ext_tick(self, now):
        """Start at most one bounded EXT read; the control loop never awaits it.

        This is deliberately stricter than "no write in flight": a read is only
        started in an idle window (no queued packet, no commanded motion, no STOP
        barrier) so an EXT ATT read can never add latency to a renewal or a STOP.
        EXT counters are cumulative, so the first read after a fault still shows
        what happened during it.
        """
        try:
            if not self.ext or not self._diag().ext_enabled: return
            task = self._ext_task
            if task is not None:
                if not task.done(): return
                self._ext_task = None
            if self.channel.writing is not None or self.channel.pending is not None: return
            if self.held is not None or self.channel.barrier: return
            if now - getattr(self, '_ext_at', 0) < 1.0: return
            self._ext_at = now
            self._ext_task = asyncio.create_task(self._poll_ext(now))
        except Exception:
            pass

    async def _poll_ext(self, now):
        client = self.client
        if client is None: return
        # Read-only, throwaway owner: a hung EXT read must not fence reconnect.
        owner = getattr(self, '_ext_owner', None) or SimpleNamespace()
        for name, _uuid in EXT_UUIDS:
            characteristic = (self.ext or {}).get(name)
            if characteristic is None: continue
            try:
                data = await bounded_await(owner, client.read_gatt_char(characteristic),
                                           getattr(self, 'ext_read_timeout', 1.0))
            except asyncio.CancelledError:
                self._diag().ext_read(name, None, 'CancelledError')
                raise
            except Exception as exc:
                self._diag().ext_read(name, None, type(exc).__name__)
                continue
            self._diag().ext_read(name, data, None)

    def receive_status(self, sender, data):
        self.emit('raw', bytes(data))
        self._diag().notify('status', len(data))
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
        self._diag().status_ack(self.last_status, s)
        if s.uart and not s.online and (initial or not previous.uart or previous.online):
            self.stats['offline'] += 1
            self.report('BLE 仍连接：S3 未收到 STM32 有效串口回包，已锁定运动')
        reset_seq = getattr(self, 'limit_reset_seq', None)
        reset_reply = reset_seq is not None and s.stop_feedback and s.ack == s.stm_ack == reset_seq
        restored = (not initial and s.uart and s.online and
                    (not previous.uart or not previous.online or not previous.can_move and s.can_move))
        if restored and not reset_reply:
            self.stats['recovered'] += 1
            self.report('STM32 串口已恢复；重新确认停止，原运行不会自动续跑')
        needs_stop = initial and s.flags & 1 or (restored and not reset_reply) or (previous and previous.can_move and not s.can_move)
        if needs_stop:
            self.report('自动停止保护：错误码 %s，ESP_ACK=%s，STM_ACK=%s；等待本次 STOP 双端确认'%(s.error,s.ack,s.stm_ack))
            self.stop(force=True)

        self.channel.status(s)
        if reset_reply and self.channel.synced and not self.channel.barrier:
            self.limit_reset_seq = None
            self.report('限位复位已确认，保持停止，需手动启动' if s.error == 0 else '限位仍锁定：请释放两路输入并等待稳定及停止确认后重新复位')
        if (self.soft_stopping and s.version==4 and s.requested==3 and s.phase==0 and s.motion==0 and
            s.current_hz==0 and s.ack==s.stm_ack and s.flags&32 and s.error==0 and not self.channel.unconfirmed):
            self.stop() # Fresh hard STOP barrier after the deceleration has completed.
            self.report('减速停止已完成，正在确认停止')
        health_key = (s.flags, s.error, self.channel.barrier, s.motion, s.requested, s.phase, s.speed, s.target_hz)
        if health_key != self.last_health_key:
            logging.getLogger('targetcarrier.link').info('flags=0x%02X error=%s ESP_ACK=%s STM_ACK=%s stop=%s requested=%s motion=%s phase=%s hz=%s target=%s stop_seq=%s',
                s.flags, s.error, s.ack, s.stm_ack, self.channel.barrier,s.requested,s.motion,s.phase,s.current_hz,s.target_hz,self.channel.stop_seq)
            self.last_health_key = health_key
            self.emit_health()
        self.emit('status', s)
    def receive_diagnostic(self,data):
        self._diag().notify('diag', len(data))
        try:detail=parse_diagnostic(data)
        except ValueError as e:
            logging.getLogger('targetcarrier.link').warning('%s',e);return
        self._diag().full_diag(time.monotonic(), detail, bytes(data))
        key=bytes(data)
        if key!=getattr(self,'last_diagnostic_key',None):
            self.last_diagnostic_key=key
            logging.getLogger('targetcarrier.link').info('ESP first-fault snapshot %s',detail)
            self.emit('fault_detail',detail)
    def receive_pulse(self, sender, data):
        self._diag().notify('pulse', len(data))
        capture = self.pulses.receive(data)
        if capture:
            self.emit('pulse', capture)
            self._diag().pulse_capture(time.monotonic(), capture)
        now = time.monotonic()
        if capture or now - self.last_pulse_event >= .2:
            self.last_pulse_event = now
            self.emit('pulse_state', self.pulses.state + '；异常 ' + str(self.pulses.errors))
