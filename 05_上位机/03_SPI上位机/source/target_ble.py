"""Independent BLE owner for target bridge. No HTTP or ESP-NOW master needed."""
import asyncio
import copy
import logging
import math
import threading
import time
from collections import deque
from ble_connect import open_ble, discover_devices, device_matches, connection_error
from target_wire import SERVICE, COMMAND, STATUS, control, gains, decode
from target_motion import TargetMotion
from target_parameters import TargetParameters


class BleTargetAdapter:
    def __init__(self, client_factory=None, scanner=None, clock=time.monotonic, background=True):
        self.clock=clock;self.factory=client_factory;self.scanner=scanner
        self.lock=threading.RLock();self.logs=deque(maxlen=120);self.samples=deque(maxlen=320)
        self.devices={};self.device_rows=[];self.endpoint=None;self.client=None
        self.generation=0;self.failures=0;self.dropped=0;self.connected=False;self.locked=True
        self.phase='未连接';self.notice='搜索 TC-Target，连接靶车桥接 ESP；不再填写 IP'
        self.planner=TargetMotion();self.kp=0.;self.kd=0.;self.gains_configured=False
        self.parameters=TargetParameters(clock);self.rx_counts={};self.last_control_hex='';self.tx_count=0
        self.last_unsolicited_log=-1e9;self.parameter_logs_suppressed=0
        self.firmware_alignment_confirmed=False
        self.en=False;self.estop=True;self.arm_token=0;self.seq=0;self.revision=0
        self.arm_confirmed=False
        self.clear_error=False;self.clear_sent_at=None;self.stop_waiting=False;self.last_diagnostic=None
        self.health=self.stm=self.motor=None;self.health_at=self.stm_at=self.motor_at=None
        self.identity=None;self.rx_seq={};self.tx_history=deque(maxlen=100)
        self.pending=None;self.pending_at=0;self.acked_revision=-1;self.last_alive=-1e9;self.focused=False
        self.last_target=(0.,0.,0.,0.);self.last_tx=0;self.last_gains=-1e9;self.initial_hold=False
        self.closed=False;self.manual_disconnect=False;self.operation=None;self.scan_task=None
        self.ever_connected=False
        self.gains_dirty=True;self.gatt_rtt_ms=0;self.control_gap_ms=0;self.last_control_at=None
        self.enable_at=None
        self.loop=None;self.thread=None;self.log=logging.getLogger('targetcarrier.link')
        if background:
            self.loop=asyncio.new_event_loop()
            self.thread=threading.Thread(target=self._run_owner,name='target-ble-owner',daemon=True)
            self.thread.start()

    def _run_owner(self):
        asyncio.set_event_loop(self.loop)
        try:
            self.loop.run_forever()
        finally:
            # Await cancellation so a connecting client can release its GATT
            # resources before the event loop disappears.
            tasks=asyncio.all_tasks(self.loop)
            for task in tasks:task.cancel()
            if tasks:self.loop.run_until_complete(asyncio.gather(*tasks,return_exceptions=True))
            self.loop.run_until_complete(self.loop.shutdown_asyncgens())
            self.loop.close()

    def _log(self, kind, text):
        self.logs.append(dict(time=time.strftime('%H:%M:%S'),kind=kind,text=text))
        self.log.info('target BLE %s: %s',kind,text)

    def alive(self, focused):
        with self.lock:
            self.focused=focused is True
            if self.focused:self.last_alive=self.clock()
            elif self.en or self.pending:self.stop('窗口未激活，旧操作作废')

    def stop(self, reason='用户停止靶车', disconnect=False):
        with self.lock:
            self.generation+=1;self.revision+=1
            self.en=False;self.estop=True;self.locked=True;self.pending=None
            self.clear_error=False;self.clear_sent_at=None;self.stop_waiting=True
            self.enable_at=None
            self.gains_dirty=True
            self.parameters.cancel(reason)
            self.arm_confirmed=False
            self.planner.hold((self.stm or {}).get('yaw'),(self.stm or {}).get('pitch'))
            self.last_target=(self.planner.yaw,self.planner.pitch,0.,0.)
            self.notice=reason+'；已请求急停/失能，等待控制器反馈'
            self.phase='停止待确认' if self.connected else '未连接 / 停止未知'
            if disconnect:self.manual_disconnect=True
            self._log('保护',self.notice)
            if disconnect and not self.connected:
                if self.operation:self.operation.cancel()
                if self.scan_task:self.scan_task.cancel()
                # Keep the endpoint reserved until the owner's finally block
                # releases GATT; a new connection must not race old cleanup.
                self.phase='正在取消靶车连接 / 搜索'
        return True

    def scan(self):
        with self.lock:
            if self.endpoint or self.scan_task and not self.scan_task.done():raise ValueError('请先断开靶车或等待搜索结束')
            if self.loop is None:raise RuntimeError('BLE owner 未启动')
            self.phase='正在搜索靶车 BLE';self.scan_task=asyncio.run_coroutine_threadsafe(self._scan(),self.loop)
            self.devices={};self.device_rows=[]
        return True

    async def _scan(self):
        try:
            if self.scanner is None:
                from bleak import BleakScanner
                self.scanner=BleakScanner
            found=await discover_devices(self.scanner,timeout=6,return_adv=True)
            devices={key:dev for key,(dev,adv) in found.items() if device_matches(dev,adv,SERVICE,('TC-Target',))}
            with self.lock:
                self.devices=devices;self.device_rows=[dict(key=k,name=d.name or 'TC-Target',transport='BLE') for k,d in devices.items()]
                self.phase='搜索结束'
                self.notice='请选择靶车设备连接' if devices else '未找到靶车 BLE 服务；需要烧录本次靶车桥接 BLE 固件'
        except Exception as exc:
            with self.lock:self.phase='搜索失败';self.notice=f'{type(exc).__name__}: {exc}';self._log('错误',self.notice)

    def connect(self,key):
        with self.lock:
            if key not in self.devices:raise ValueError('请先搜索并选择靶车 BLE 设备')
            if self.endpoint or self.operation and not self.operation.done():raise ValueError('靶车已有连接操作')
            self.endpoint=key;self.manual_disconnect=False;self.phase='正在连接靶车 BLE'
            self.ever_connected=False
            self.operation=asyncio.run_coroutine_threadsafe(self._connect_loop(self.devices[key]),self.loop)
        return True

    async def _connect_loop(self,device):
        try:
            attempt=0
            while not self.closed and not self.manual_disconnect:
                try:await self._session(device)
                except asyncio.CancelledError:return
                except Exception as exc:
                    with self.lock:
                        self.failures+=1;self.notice='靶车 '+connection_error(exc);self._log('错误',self.notice)
                    self.log.exception('Target BLE session failed')
                with self.lock:
                    if self.manual_disconnect or self.closed or not self.ever_connected:break
                    self.phase='靶车连接失效，准备重连；保持锁定'
                await asyncio.sleep(min(8,.5*2**min(attempt,4)));attempt+=1
        finally:
            with self.lock:
                self.endpoint=None
                if self.manual_disconnect:self.phase='靶车连接已取消 / 断开'
                else:self.phase='靶车连接已结束';self.notice+='；需要新启动命令，不恢复旧运动'

    def _connection_report(self,message):
        with self.lock:self.phase=message;self._log('连接',message)

    def _fresh_age(self, stamp):return 65535 if stamp is None else max(0,round((self.clock()-stamp)*1000))

    def _status(self):
        ha=self._fresh_age(self.health_at);sa=self._fresh_age(self.stm_at);ma=self._fresh_age(self.motor_at)
        health=self.health or {};stm=self.stm or {};motor=self.motor or {}
        # Health is published every 200 ms, STM/motor every 100 ms. A newer
        # validated downstream frame supersedes the older health transit age.
        # Retain the bridge's reported source age as a conservative offset,
        # and never let health alone refresh a missing downstream frame.
        sa=max(sa,health.get('age',65535)+min(ha,sa))
        ma=max(ma,health.get('motor_age',65535)+min(ha,ma))
        st_fresh=bool(self.connected and ha<800 and sa<=300 and health.get('flags',0)&2)
        motor_fresh=bool(self.connected and ha<800 and ma<=300 and not motor.get('safety',16)&16)
        safety=motor.get('safety',0)
        enabled=(1 if safety&2 else 0) if motor_fresh else -1
        arm=int(self.en and motor_fresh and safety&1 and motor.get('token')==self.arm_token)
        raw_err=stm.get('error',0);error=raw_err if raw_err>=8 else 0
        if stm.get('state')==4 or safety&8:error=error or 1
        safe=not st_fresh or not motor_fresh or bool(health.get('flags',4)&4) or bool(safety&(32|64)) or bool(error)
        return dict(en=int(self.en),estop=int(self.estop),up=self.clock(),yaw=self.last_target[0],pitch=self.last_target[1],
            st_age=sa,br_age=ha,st_fresh=int(st_fresh),st_online=int(st_fresh),st_state=stm.get('state',-1),st_err=error,
            st_raw_err=raw_err,st_yaw=stm.get('yaw'),st_temp=stm.get('temp'),enabled=enabled,arm=arm,safe=int(safe),
            motor_fresh=int(motor_fresh),motor_safety=safety,raw_armed=int(bool(safety&1)),
            gatt_rtt_ms=self.gatt_rtt_ms,control_gap_ms=self.control_gap_ms,gains_pending=self.gains_dirty,
            gains_configured=self.gains_configured,gains_feedback='仅已发送；现有固件不回传 kp/kd，设备采用值未确认',
            firmware_alignment_confirmed=self.firmware_alignment_confirmed,
            bridge_rejected=health.get('rejected',0),uart_errors=health.get('uart_errors',0),cmd_drops=motor.get('drops'),
            m_pos=motor.get('pos'),m_vel=motor.get('vel'),m_tau=motor.get('tau'),motor_age=ma,
            arm_token=motor.get('token',0),kp=self.kp,kd=self.kd,mode=TargetMotion.NAMES[self.planner.profile],
            vel=int(self.planner.velocity),cruise=int(self.planner.cruise),ry=self.planner.yaw_rate if self.planner.velocity else 0.,
            rp=self.planner.pitch_rate if self.planner.velocity else 0.,br_flags=health.get('flags',0),can_fail=motor.get('can_fail'))

    def _controller_stopped(self,s):
        return bool(s['st_fresh'] and s['enabled']==0 and not s['raw_armed'] and s['st_state'] in (0,3) and
                    type(s['m_vel']) in (int,float) and abs(s['m_vel'])<=.05)

    def _clear_allowed(self,s):
        return bool(not self.en and s['st_fresh'] and s['motor_fresh'] and s['enabled']==0 and
                    s['st_state'] in (0,3,4) and
                    not s['raw_armed'] and type(s['m_vel']) in (int,float) and abs(s['m_vel'])<=.05 and
                    not s['br_flags']&4 and not s['motor_safety']&(32|64))

    def _feedback_notice(self,s):
        if not s['st_fresh']:return 'BLE 在线，STM32 反馈过期；检查桥接 RX → STM32 TX、921600 波特率及供电'
        if not s['motor_fresh']:return 'STM32 在线，电机反馈过期；检查 CAN 接线、电机供电及 ID'
        if s['motor_safety']&64:return '电机反馈在线，但 STM32 未收到新鲜控制指令；检查桥接 TX → STM32 PA10 和 UART 接收'
        if s['motor_safety']&32:return 'STM32 正在恢复 CAN 总线；保持失能，检查 CAN 接线和终端电阻'
        if s['st_raw_err']>=8:return f"电机自报故障 {s['st_raw_err']}；排除原因后手动清错"
        if s['st_err']:return '反馈已恢复，STM32 故障仍锁存；确认失能、零速后点击“清除故障（保持失能）”'
        if s['br_flags']&4:return '桥接安全保护中，等待停止指令被桥接接受'
        if self._controller_stopped(s):return 'STM32 / 电机已反馈失能、零速；仍保持软件锁定，需手动解除急停后再使能'
        return '已收到 STM32 和电机反馈，等待失能、撤销授权及零速确认'

    def _receive(self,token,data):
        with self.lock:
            if token is not self.connection_token:return
            try:kind,value=decode(data)
            except ValueError as exc:
                self.dropped+=1;self._log('校验',str(exc));return
            now=self.clock()
            self.rx_counts[kind]=self.rx_counts.get(kind,0)+1
            if kind=='health':
                identity=(value['boot'],value['epoch'])
                if self.identity is not None and identity!=self.identity:
                    self.stop('靶车桥接复位或会话改变');self.parameters.cancel('会话改变',invalidate=True)
                    self.stm=self.motor=None;self.stm_at=self.motor_at=None;self.rx_seq.clear()
                self.identity=identity;self.health=value;self.health_at=now
                for seq,rev,at in self.tx_history:
                    if (value['ctrl_seq']-seq)&255<128 and now-at<.8:self.acked_revision=max(self.acked_revision,rev)
            elif kind in ('stm','motor'):
                old=self.rx_seq.get(kind);seq=value['seq']
                if old is not None and not 0<(seq-old)&255<128:return
                self.rx_seq[kind]=seq
                if kind=='stm':self.stm=value;self.stm_at=now
                else:self.motor=value;self.motor_at=now
            else:
                if self.parameters.receive(value):self._log('参数',self.parameters.result)
                elif now-self.last_unsolicited_log>=.5:
                    self.last_unsolicited_log=now;self._log('参数',str(value))
                else:self.parameter_logs_suppressed+=1
                if self.pending in ('read_config','read_parameter','write_parameter') and not self.parameters.busy:
                    self.pending=None;self.notice=self.parameters.result
            s=self._status()
            diagnostic=(s['st_state'],s['st_raw_err'],s['motor_safety'],s['st_fresh'],s['motor_fresh'],s['br_flags'])
            if kind=='health' and diagnostic!=self.last_diagnostic:
                self.last_diagnostic=diagnostic
                self._log('反馈',f"state={s['st_state']} motor_err={s['st_raw_err']} safety=0x{s['motor_safety']:02x} "
                          f"stm_age={s['st_age']} motor_age={s['motor_age']} bridge=0x{s['br_flags']:02x} "
                          f"uart_errors={s['uart_errors']} cmd_drops={s['cmd_drops']}")
            if self.stop_waiting and self._controller_stopped(s) and not s['st_err']:
                self.notice=self._feedback_notice(s);self.stop_waiting=False
            if not self.initial_hold and self._controller_stopped(s):
                self.planner.hold(s['st_yaw'],0);self.initial_hold=True
            if self.en and (not s['st_fresh'] or s['st_err'] or s['safe']):
                self.stop(f"靶车反馈过期或保护：stm_age={s['st_age']} motor_age={s['motor_age']} "
                          f"state={s['st_state']} error={s['st_raw_err']} safety=0x{s['motor_safety']:02x} bridge=0x{s['br_flags']:02x}")
            if self.en and self.arm_confirmed and (s['enabled']!=1 or s['arm']!=1):self.stop('STM32 授权/电机使能反馈丢失，需重新手动使能')
            if self.en and s['enabled']==1 and s['arm']==1:self.arm_confirmed=True
            if self.pending and self.pending not in ('read_config','read_parameter','write_parameter') and self.acked_revision>=self.revision:
                ready=True
                if self.pending=='release':ready=self._controller_stopped(s) and s['st_state']==0
                elif self.pending=='enable':ready=s['enabled']==1 and s['arm']==1 and s['st_state']==1
                elif self.pending=='clear_error':
                    # Do not accept the bridge ACK or the STM32's 500 ms old-fault
                    # mask as recovery. Require both downstream frames after it.
                    after=None if self.clear_sent_at is None else self.clear_sent_at+.65
                    ready=bool(after is not None and self.stm_at is not None and self.motor_at is not None and
                               min(self.stm_at,self.motor_at)>=after and self._clear_allowed(s) and
                               s['st_state']==0 and not s['st_err'])
                if ready:
                    if self.pending=='release':
                        self.locked=False;self.notice='已确认解除急停，电机仍失能；需要手动点击使能'
                    if self.pending=='enable':self.notice='STM32 授权和电机使能已反馈确认'
                    if self.pending=='clear_error':
                        self.estop=True;self.clear_sent_at=None;self.locked=True
                        self.notice='已确认故障清除，仍保持失能和软件锁定；需要手动解除急停，再手动使能'
                    self._log('回报',self.pending+' 控制链路已接受；运动/使能另看 STM32 反馈')
                    self.pending=None
            if kind=='stm':self.samples.append(dict(at=time.time(),**s))

    def command(self,name,values=None):
        if name in ('stop','disable'):return self.stop()
        if name=='disconnect':return self.stop('断开靶车前停止',disconnect=True)
        values=values or {}
        def number(key,lo,hi):
            v=values.get(key)
            if type(v) not in (int,float) or not math.isfinite(v) or not lo<=v<=hi:raise ValueError(f'{key} 范围 {lo}–{hi}')
            return v
        with self.lock:
            s=self._status()
            if not self.connected or self._fresh_age(self.health_at)>800:raise ValueError('请先连接靶车 BLE 并等待桥接状态')
            if not self.focused or self.clock()-self.last_alive>.7:raise ValueError('窗口未激活')
            if self.pending:raise ValueError('上一操作等待回报，请稍候')
            if name=='confirm_firmware':
                if self.en or not self._controller_stopped(s) or not self.parameters.config_verified:
                    raise ValueError('先在失能状态读取电机配置，确认量程一致')
                self.firmware_alignment_confirmed=True
                self.notice='已记录人工确认：当前 STM32 已实际烧录 VMAX45 版；软件不能自动读回固件常量'
                self._log('人工确认',self.notice)
                return True
            if name in ('read_config','read_parameter','write_parameter'):
                if self.en or not self._controller_stopped(s):raise ValueError('参数访问需要新鲜失能、无授权及零速反馈')
                self.parameters.start(name,values)
                self.notice='参数请求排队；写入不自动重试、不自动保存'
            elif name=='release':
                if not self.locked or not self._controller_stopped(s) or s['st_err'] or s['safe']:raise ValueError('先确认 STM32 和电机已失能、停止且无故障')
                self.estop=False
            elif name=='clear_error':
                if not self._clear_allowed(s):raise ValueError('清错需要 STM32 / 电机新鲜反馈、失能、无授权、零速，且 UART / CAN 正常')
                if not s['st_err']:raise ValueError('当前没有故障锁存，无需清错')
                self.clear_error=True;self.clear_sent_at=None;self.locked=True;self.stop_waiting=False
                self.planner.hold(s['st_yaw'],0)
                self.notice='清错已请求；保持失能，等待故障屏蔽期后的 STM32 和电机反馈'
            elif name=='gains':
                if self.en or not self._controller_stopped(s):raise ValueError('修改增益前请先停止并确认失能，增益发送不能阻塞运动心跳')
                self.kp,self.kd=number('kp',0,200),number('kd',0,3);self.gains_dirty=True
                self.gains_configured=self.kp>0 and self.kd>=.01
                self._log('参数',f'请求增益 kp={self.kp} kd={self.kd}；固件不提供采用值回读')
            elif self.locked:raise ValueError('先手动解除急停，恢复连接不会自动使能')
            elif name=='enable':
                if not self.parameters.config_verified:raise ValueError('先读取电机配置，核对 PMAX/VMAX/TMAX 与当前固件一致')
                if not self.firmware_alignment_confirmed:raise ValueError('当前无法读回 STM32 固件版本；实际烧录 VMAX45 版后再人工确认，不能只修改源码')
                if not self.gains_configured:raise ValueError('先在失能状态明确应用已确认的 kp/kd；不使用默认高增益')
                if self.gains_dirty:raise ValueError('正在发送增益，请等待完成后再使能')
                if self.en or s['safe'] or not self._controller_stopped(s) or s['st_state']!=0 or self.kp<=0 or self.kd<.01:
                    raise ValueError('需要下游新鲜停止反馈、解除急停及有效 kp/kd')
                if type(s['st_yaw']) not in (int,float) or not -180<=s['st_yaw']<=180:
                    raise ValueError('实际位置超出当前软限位；先核对零点、量程与机械范围，禁止夹紧后使能')
                # Arming only holds the latest actual pose. Old angles, rates,
                # sine/sweep/cruise selections never start by clicking ENABLE.
                self.planner.hold(s['st_yaw'],0);self.last_target=(self.planner.yaw,0.,0.,0.)
                self.arm_token=(s['arm_token']+1)&255 or 1;self.en=True;self.enable_at=self.clock()
            elif name=='angles':self.planner.angles(number('yaw',-180,180),number('pitch',-30,60))
            elif name in ('velocity','program_velocity'):self.planner.rates(number('yaw_rate',-90,90),number('pitch_rate',-60,60))
            elif name=='profile':
                v=number('profile',0,5)
                if int(v)!=v:raise ValueError('轨迹模式必须为整数')
                self.planner.select(int(v))
            elif name=='sine':self.planner.sine_amp,self.planner.period=number('amplitude',0,180),number('period',.2,600)
            elif name=='sweep':self.planner.sweep_amp,self.planner.sweep_speed=number('amplitude',0,180),number('speed',.1,180)
            elif name=='cruise':
                speed=number('speed',-90,90);self.planner.select(5)
                self.planner.cruise=bool(speed);self.planner.cruise_speed=abs(speed);self.planner.cruise_dir=-1 if speed<0 else 1
            elif name=='program_prepare':
                if s['enabled']!=0:raise ValueError('清除旧轨迹前必须确认失能')
                self.planner.rates(0,0)
            else:raise ValueError('不支持的靶车操作')
            self.revision+=1;self.pending=name;self.pending_at=self.clock()
            self._log('操作',f'{name} 已提交 values={values} target={self.last_target} kp={self.kp} kd={self.kd}')
        return True

    async def _session(self,device):
        if self.factory is None:
            from bleak import BleakClient
            self.factory=BleakClient
        token=object();self.connection_token=token;client=None
        with self.lock:
            self.stop('新 BLE 会话先停止');self.identity=None;self.health=self.stm=self.motor=None
            self.health_at=self.stm_at=self.motor_at=None;self.rx_seq.clear();self.tx_history.clear();self.initial_hold=False
            self.gains_dirty=True;self.last_control_at=None
            self.parameters.cancel('新会话',invalidate=True);self.gains_configured=False
            self.firmware_alignment_confirmed=False
        try:
            if isinstance(device,str) and device.startswith('usb:'):
                client=self.factory(device);await client.connect()
                client.on_link_loss=lambda reason:self.stop(reason)
            else:client=await open_ble(self.factory,device,SERVICE,self._connection_report)
            self.client=client
            self._connection_report('靶车服务已确认，正在订阅 BLE 状态通知')
            await asyncio.wait_for(client.start_notify(STATUS,lambda sender,data:self._receive(token,data)),5)
            with self.lock:self.connected=True;self.ever_connected=True;self.phase='靶车 BLE 在线，等待 STM32 停止反馈';self.last_tx=self.clock()
            last_read=self.clock();last_step=self.clock()
            while client.is_connected and not self.closed:
                now=self.clock()
                if self.manual_disconnect:
                    with self.lock:
                        self.seq=(self.seq+1)&255
                        packet=control(self.seq,8,self.planner.yaw,self.planner.pitch)
                    await asyncio.wait_for(client.write_gatt_char(COMMAND,packet,response=True),.5)
                    await asyncio.sleep(.1)
                    break
                with self.lock:
                    if self.en and (not self.focused or now-self.last_alive>.7):self.stop('界面心跳失效')
                    limit=10 if self.pending in ('read_config','read_parameter','write_parameter') else 2.5 if self.pending=='clear_error' else 1.5
                    if self.pending and now-self.pending_at>limit:
                        name=self.pending;self.stop(f'{name} 下游确认超时：'+self._feedback_notice(self._status()))
                    dt=now-last_step
                    if self.en and self.enable_at is not None:dt=min(dt,now-self.enable_at)
                    if self.en and dt>.25:self.stop('电脑控制调度停顿，旧运动作废')
                    last_step=now
                    target=self.planner.step(dt) if self.en else (self.planner.yaw,self.planner.pitch,0.,0.)
                    self.last_target=target
                    flags=(1|16|(self.arm_token<<8)) if self.en else 0
                    if self.estop:flags|=8
                    if self.pending=='clear_error':
                        # STM32 returns early on ESTOP. Clear without ESTOP or
                        # ARM/ENABLE, then hold disabled until real confirmation.
                        flags=2 if self.clear_error else 0
                        self.clear_error=False
                    self.seq=(self.seq+1)&255;seq=self.seq;revision=self.revision;generation=self.generation
                    packet=control(seq,flags,*target);created=self.clock()
                with self.lock:valid=generation==self.generation and self.clock()-created<=.1
                if valid:
                    with self.lock:
                        self.control_gap_ms=0 if self.last_control_at is None else round((self.clock()-self.last_control_at)*1000)
                        self.last_control_at=self.clock()
                        self.last_control_hex=packet.hex();self.tx_count+=1
                    await self._write(client,packet,'控制')
                    with self.lock:
                        self.tx_history.append((seq,revision,now))
                        if flags&2 and generation==self.generation and self.pending=='clear_error':self.clear_sent_at=self.clock()
                if self.gains_dirty and not self.en and (not hasattr(client,"transport_ready") or client.transport_ready()):
                    with self.lock:g=gains(self.seq,self.kp,self.kd);sent_gains=(self.kp,self.kd)
                    await self._write(client,g,'增益')
                    with self.lock:
                        if sent_gains==(self.kp,self.kd):self.gains_dirty=False
                        self.last_gains=self.clock()
                        self._log('发送',f'GAINS kp={sent_gains[0]} kd={sent_gains[1]} frame={g.hex()}；GATT已接受，设备采用值未确认')
                # Low-priority, serialized register traffic is only allowed while
                # disabled; it cannot queue behind or retransmit motion commands.
                with self.lock:
                    ps=self._status();param=None;pg=self.generation
                    if self.parameters.busy and not self.en and self._controller_stopped(ps):
                        self.seq=(self.seq+1)&255;param=self.parameters.next_packet(self.seq)
                    if not self.parameters.busy and self.pending in ('read_config','read_parameter','write_parameter'):
                        self.pending=None;self.notice=self.parameters.result
                if param is not None:
                    with self.lock:send_param=pg==self.generation and self.parameters.busy and not self.en
                    if send_param:
                        await self._write(client,param,'参数')
                        with self.lock:self._log('发送',f'PARAM op={param[4]} rid={param[5]} frame={param.hex()}')
                if now-last_read>.6 and self._fresh_age(self.health_at)>600:
                    data=await asyncio.wait_for(client.read_gatt_char(STATUS),.5);self._receive(token,data);last_read=now
                # GATT write-with-response already waits for a connection event.
                # Do not add another 50 ms after it or resend unchanged gains.
                await asyncio.sleep(max(0,.05-(self.clock()-now)))
        finally:
            with self.lock:
                self.stop('靶车 BLE 会话结束');self.connected=False;self.connection_token=None
            if client:
                try:await asyncio.wait_for(client.disconnect(),2)
                except Exception:pass

    async def _write(self,client,packet,stage):
        started=self.clock()
        try:await asyncio.wait_for(client.write_gatt_char(COMMAND,packet,response=True),.5)
        except TimeoutError as exc:raise RuntimeError(f'BLE 已连接，但 GATT {stage}写入超时；保持锁定并重连') from exc
        finally:self.gatt_rtt_ms=round((self.clock()-started)*1000)

    def snapshot(self):
        with self.lock:
            s=self._status();fresh=self.connected and self._fresh_age(self.health_at)<800
            stopped=self._controller_stopped(s)
            phase=self.phase
            if self.connected and not fresh:phase='BLE 已连接，等待桥接健康帧；STM32 状态未确认'
            if fresh:phase='STM32 / 电机故障锁定' if s['st_err'] else '靶车已使能' if s['enabled']==1 else '控制器已停止' if stopped else 'BLE 在线，下游状态待确认'
            notice=self._feedback_notice(s) if fresh and (self.stop_waiting or self.locked and not self.pending) else self.notice
            if fresh and self.pending=='clear_error':phase='正在清除故障，保持失能'
            can_clear=fresh and not self.pending and bool(s['st_err']) and self._clear_allowed(s)
            can_release=fresh and not self.pending and self.locked and stopped and not s['st_err'] and not s['safe']
            return copy.deepcopy(dict(endpoint=self.endpoint,connected=fresh,locked=self.locked,busy=bool(self.pending),phase=phase,
                ble_connected=self.connected,connecting=bool(self.endpoint and not self.connected),
                notice=notice,status=s if self.health else None,historical=not fresh,age_ms=self._fresh_age(self.health_at),
                can_clear_error=bool(can_clear),can_release=bool(can_release),
                parameters=self.parameters.snapshot(),rx_counts=dict(self.rx_counts),tx_count=self.tx_count,last_control_hex=self.last_control_hex,
                parameter_logs_suppressed=self.parameter_logs_suppressed,
                generation=self.generation,failures=self.failures,dropped=self.dropped,pending=int(bool(self.pending)),
                physical_stop_confirmed=False,controller_stop_confirmed=stopped,stm_online=bool(s['st_fresh']),
                samples=list(self.samples),logs=list(self.logs),devices=self.device_rows,transport='BLE',
                scanning=bool(self.scan_task and not self.scan_task.done())))

    def close(self):
        if self.closed:return
        self.stop('退出上位机',disconnect=True)
        if self.operation:
            try:self.operation.result(timeout=3)
            except Exception:self.operation.cancel()
        self.closed=True
        if self.scan_task:self.scan_task.cancel()
        if self.loop and not self.loop.is_closed():
            self.loop.call_soon_threadsafe(self.loop.stop)
            if self.thread:self.thread.join(timeout=3)
