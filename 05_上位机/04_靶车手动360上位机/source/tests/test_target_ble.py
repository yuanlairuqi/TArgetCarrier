import asyncio
import json
import struct
import sys
import time
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from target_wire import SERVICE,COMMAND,STATUS,control,gains,decode,crc16,frame,CAPABILITY_RID,CAPABILITY_VALUE,VELOCITY_FLAG
from target_motion import TargetMotion
from target_ble import BleTargetAdapter
from ble_connect import open_ble
from test_ramp import Services


def status_frame(seq,state=3,yaw=0,error=0):
    data=bytearray([0x5a,15,2,seq&255,0,state])+bytearray(struct.pack('<ffBB',yaw,0,25,error))
    return bytes(data)+bytes([crc16(data[1:16])&255])


class SimTarget:
    """GATT and existing STM32 frames only; adapter/trajectory implementation is real."""
    def __init__(self,*a,**kw):
        self.services=Services();self.is_connected=False;self.writes=[];self.ctrl_seq=0
        self.seq=0;self.flags=8;self.token=0;self.position=0.;self.yaw=0.;self.disarm=False
        self.reject_enable=False;self.epoch=1;self.fail=False
        self.fault=False;self.persist_fault=False;self.velocity=0.;self.extra_safety=0;self.ignore_control=False;self.clear_requests=0
        self.fault_reassert_at=None
        self.write_delay=0
        self.registers={CAPABILITY_RID:struct.pack('<I',CAPABILITY_VALUE),10:struct.pack("<I",1),21:struct.pack("<f",12.5),22:struct.pack("<f",45),23:struct.pack("<f",10)}
    async def connect(self):self.is_connected=True
    async def disconnect(self):
        self.is_connected=False
        if hasattr(self,'task'):self.task.cancel();await asyncio.gather(self.task,return_exceptions=True)
    def health(self):
        return b'TB\x01'+struct.pack('<BIIHHBBH',1|2|(8 if self.disarm else 4)|(16 if self.flags&1 else 0),42,self.epoch,0,0,self.ctrl_seq,0,0)
    async def read_gatt_char(self,*a):return self.health()
    async def start_notify(self,uuid,callback):
        self.callback=callback
        async def send():
            while self.is_connected:
                if self.fault_reassert_at is not None and time.monotonic()>=self.fault_reassert_at:
                    self.fault=True;self.fault_reassert_at=None
                enabled=bool(self.flags&1 and not self.reject_enable)
                state=3 if self.flags&8 else 4 if self.fault else 1 if enabled else 0
                self.seq=(self.seq+1)&255
                callback(None,status_frame(self.seq,state,self.position,1 if enabled else 0))
                safety=(3 if enabled else 0)|(4 if state==3 else 0)|(8 if self.fault else 0)|self.extra_safety
                callback(None,frame(6,self.seq,struct.pack('<hhhHBBB',round(self.position/.01),round(self.velocity/.01),0,0,safety,0,self.token)))
                callback(None,self.health())
                await asyncio.sleep(.02)
        self.task=asyncio.create_task(send())
    async def write_gatt_char(self,uuid,data,response=True):
        if self.write_delay:await asyncio.sleep(self.write_delay)
        if self.fail:raise OSError('injected GATT loss')
        self.writes.append(bytes(data))
        if data[2]==4:
            op,rid=data[4:6]
            if op in (2,3):self.registers[rid]=bytes(data[6:10])
            raw=self.registers.get(rid,bytes(4))
            self.seq=(self.seq+1)&255
            self.callback(None,frame(5,self.seq,bytes([rid,1])+raw+bytes(5)))
        if data[2]==1:
            self.ctrl_seq=data[3]
            if self.ignore_control:return
            self.flags=struct.unpack_from('<H',data,4)[0]
            # STM32 applies ESTOP before its one-shot clear branch.
            if self.flags&2 and not self.flags&8:
                self.clear_requests+=1
                self.fault=False
                if self.persist_fault:self.fault_reassert_at=time.monotonic()+.5
            if not self.flags&1:self.disarm=True
            if self.flags&1:self.token=self.flags>>8
            self.yaw=struct.unpack_from('<h',data,7)[0]*.01
            if self.flags&1 and not self.reject_enable:self.position=self.yaw


class CodecAndMotionTests(unittest.TestCase):
    def test_wire_compatible_with_original_c_protocol(self):
        g=json.loads(Path(__file__).with_name('target_motion_golden.json').read_text(encoding='utf-8-sig'))
        self.assertEqual(control(254,0x111,-12.34,56.78,-3.2,9.1).hex(),g['control'])
        self.assertEqual(gains(9,40,1.5).hex(),g['gains'])
    def test_profiles_enter_without_large_position_jump(self):
        for profile in range(6):
            m=TargetMotion();m.hold(65,0);m.select(profile)
            self.assertLess(abs(m.step(.001)[0]-65),.1)
    def test_status_error_one_is_enabled_not_a_fault(self):
        kind,v=decode(status_frame(1,1,12,1));self.assertEqual(kind,'stm');self.assertEqual(v['error'],1)
    def test_crc_noise_and_nonfinite_feedback_rejected(self):
        b=bytearray(status_frame(1));b[8]^=1
        with self.assertRaises(ValueError):decode(b)
        with self.assertRaises(ValueError):decode(status_frame(1,yaw=float('nan')))
        with self.assertRaises(ValueError):control(1,0,181,0)
    def test_continuous_velocity_does_not_accumulate_or_limit_yaw(self):
        m=TargetMotion();m.hold(650,0);m.rates(90,0)
        for _ in range(200):
            y,p,yr,pr=m.step(.05);self.assertEqual(y,650);self.assertEqual(yr,90)
        m.hold();self.assertFalse(m.velocity);self.assertEqual(m.step(.1)[2:],(0,0))


class AdapterTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.a=BleTargetAdapter(client_factory=SimTarget,background=False)
        self.a.alive(True);self.task=asyncio.create_task(self.a._session(None))
        # Initial simulated STOP status can precede the bridge's disarm echo.
        # Wait for both real preflight requirements instead of depending on
        # whether the scheduler happened to send the first CTRL already.
        await self.until(lambda:self.a.snapshot()['controller_stop_confirmed'] and self.a.snapshot()['status']['safe']==0)
        await self.until(lambda:self.a.clock()>=self.a.parameters.quiet_until)
        self.a.command('read_config');await self.until(lambda:not self.a.pending and self.a.parameters.config_verified)
        self.a.command('gains',dict(kp=60,kd=1.5));await self.until(lambda:not self.a.pending and not self.a.gains_dirty)
        self.a.command('confirm_firmware')
    async def until(self,condition,timeout=2):
        end=time.monotonic()+timeout
        while not condition():
            self.a.alive(True)
            if self.task.done():await self.task;self.fail('session ended')
            if time.monotonic()>end:self.fail(self.a.snapshot())
            await asyncio.sleep(.02)
    async def asyncTearDown(self):
        self.a.closed=True
        self.task.cancel()
        await asyncio.wait_for(asyncio.gather(self.task,return_exceptions=True),2)
    async def release(self):
        self.a.command('release');await self.until(lambda:not self.a.locked and not self.a.pending)
    async def enable(self):
        await self.release();self.a.command('enable');await self.until(lambda:not self.a.pending and self.a.snapshot()['status']['enabled']==1)
    async def test_connect_disarms_only_and_waits_for_explicit_enable(self):
        self.assertTrue(self.a.locked);self.assertFalse(self.a.en)
        self.assertTrue(all(not struct.unpack_from('<H',d,4)[0]&1 for d in self.a.client.writes if d[2]==1))
    async def test_release_never_enables(self):
        await self.release();self.assertFalse(self.a.en);self.assertEqual(self.a.snapshot()['status']['enabled'],0)
    async def test_enable_uses_actual_arm_echo_and_enabled_feedback(self):
        await self.enable();s=self.a.snapshot()['status']
        self.assertEqual(s['arm'],1);self.assertEqual(s['st_err'],0);self.assertEqual(s['enabled'],1)
    async def test_declined_enable_does_not_become_success_on_gatt_ack(self):
        await self.release();self.a.client.reject_enable=True;self.a.command('enable')
        await self.until(lambda:self.a.locked and not self.a.en,timeout=3)
        self.assertNotEqual(self.a.snapshot()['status']['enabled'],1)
    async def test_all_existing_controls_remain_available(self):
        await self.release();self.a.command('gains',dict(kp=40,kd=1.5));await self.until(lambda:not self.a.pending and not self.a.gains_dirty)
        self.a.command('enable');await self.until(lambda:not self.a.pending)
        for name,args in [('angles',dict(yaw=20,pitch=10)),('velocity',dict(yaw_rate=10,pitch_rate=0)),
                          ('profile',dict(profile=1)),('sine',dict(amplitude=20,period=4)),
                          ('sweep',dict(amplitude=30,speed=10)),('cruise',dict(speed=-10))]:
            self.a.command(name,args);await self.until(lambda:not self.a.pending)
        self.assertTrue(self.a.planner.cruise)
    async def test_stop_discards_trajectory_and_does_not_resume(self):
        await self.enable();self.a.command('velocity',dict(yaw_rate=20,pitch_rate=0));await self.until(lambda:not self.a.pending)
        self.a.stop();await self.until(lambda:self.a.snapshot()['controller_stop_confirmed'])
        count=len(self.a.client.writes);await asyncio.sleep(.15)
        self.assertTrue(self.a.locked);self.assertFalse(self.a.planner.velocity)
        self.assertTrue(all(not struct.unpack_from('<H',d,4)[0]&1 for d in self.a.client.writes[count:] if d[2]==1))
    async def test_ui_lease_locks_but_keeps_ble_session(self):
        await self.enable();self.a.alive(False);await asyncio.sleep(.1)
        self.assertTrue(self.a.locked);self.assertTrue(self.a.client.is_connected)
    async def test_epoch_change_invalidates_authorization(self):
        await self.enable();generation=self.a.generation;self.a.client.epoch+=1
        await self.until(lambda:self.a.generation>generation)
        self.assertTrue(self.a.locked);self.assertFalse(self.a.en)
    async def test_downstream_reset_cannot_keep_old_enable_authorization(self):
        await self.enable();self.a.client.reject_enable=True
        await self.until(lambda:self.a.locked and not self.a.en)
        self.a.client.reject_enable=False
        await asyncio.sleep(.15)
        self.assertFalse(self.a.en)
    async def test_late_callback_cannot_revive_old_session(self):
        old=self.a.connection_token;self.a.connection_token=object()
        self.a._receive(old,status_frame(200,1,120,1))
        self.assertNotEqual((self.a.stm or {}).get('yaw'),120)
    async def test_invalid_input_never_enables(self):
        await self.release()
        for name,args in [('angles',dict(yaw=181,pitch=0)),('velocity',dict(yaw_rate=float('nan'),pitch_rate=0)),('gains',dict(kp=201,kd=1))]:
            with self.assertRaises(ValueError):self.a.command(name,args)
        self.assertFalse(self.a.en)

    async def fault(self):
        self.a.client.fault=True
        await self.until(lambda:self.a.snapshot()['status']['st_err']==1)

    async def test_clear_is_disabled_only_and_waits_past_old_fault_mask(self):
        await self.fault();self.assertTrue(self.a.snapshot()['can_clear_error'])
        self.a.command('clear_error');started=time.monotonic()
        await self.until(lambda:self.a.client.clear_requests==1)
        await asyncio.sleep(.2);self.a.alive(True)
        self.assertEqual(self.a.pending,'clear_error')
        await self.until(lambda:not self.a.pending)
        self.assertGreaterEqual(time.monotonic()-started,.65)
        self.assertTrue(self.a.locked);self.assertFalse(self.a.en);self.assertTrue(self.a.estop)
        flags=[struct.unpack_from('<H',d,4)[0] for d in self.a.client.writes if d[2]==1]
        self.assertTrue(any(f==2 for f in flags));self.assertTrue(all(not f&(1|16) for f in flags))
        self.assertEqual(self.a.client.clear_requests,1)
        await self.until(lambda:self.a.snapshot()['can_release'])
        await self.release();self.assertFalse(self.a.en)

    async def test_clear_requires_zero_speed_and_no_raw_arm(self):
        await self.fault()
        for vel,safety in ((.1,0),(0,1),(0,16),(0,32),(0,64)):
            self.a.client.velocity=vel;self.a.client.extra_safety=safety
            await self.until(lambda:self.a.snapshot()['status']['motor_safety']&safety==safety and self.a.snapshot()['status']['m_vel']==vel)
            with self.assertRaises(ValueError):self.a.command('clear_error')
        self.assertEqual(self.a.client.clear_requests,0)

    async def test_persistent_fault_cannot_succeed_on_bridge_ack(self):
        await self.fault();self.a.client.persist_fault=True;self.a.command('clear_error')
        await self.until(lambda:not self.a.pending,timeout=4)
        self.assertTrue(self.a.locked);self.assertTrue(self.a.estop);self.assertFalse(self.a.en)
        self.assertEqual(self.a.snapshot()['status']['st_err'],1)
        self.assertFalse(any(row['kind']=='回报' and row['text'].startswith('clear_error') for row in self.a.logs))

    async def test_stop_cancels_clear_before_transmission(self):
        await self.fault();self.a.command('clear_error');self.a.stop()
        await asyncio.sleep(.15)
        self.assertEqual(self.a.client.clear_requests,0);self.assertFalse(self.a.clear_error)
        self.assertTrue(self.a.estop);self.assertTrue(self.a.locked)

    async def test_stop_interrupts_pending_clear_without_retry(self):
        await self.fault();self.a.command('clear_error')
        await self.until(lambda:self.a.client.clear_requests==1)
        self.a.stop();await asyncio.sleep(.15)
        self.assertIsNone(self.a.pending);self.assertTrue(self.a.estop)
        self.assertEqual(self.a.client.clear_requests,1)

    async def test_release_rejects_latched_fault_even_when_estop_reported(self):
        await self.fault()
        self.assertFalse(self.a.snapshot()['can_release'])
        with self.assertRaises(ValueError):self.a.command('release')

    async def test_no_fault_does_not_offer_clear_and_stop_notice_is_confirmed(self):
        self.assertFalse(self.a.snapshot()['can_clear_error'])
        with self.assertRaises(ValueError):self.a.command('clear_error')
        self.assertNotIn('等待控制器反馈',self.a.snapshot()['notice'])

    async def test_lost_command_direction_is_distinct_from_live_motor_feedback(self):
        self.a.client.extra_safety=64;self.a.stop()
        await self.until(lambda:bool(self.a.snapshot()['status']['motor_safety']&64))
        self.assertIn('STM32 未收到新鲜控制指令',self.a.snapshot()['notice'])
        self.assertTrue(self.a.snapshot()['status']['motor_fresh'])

    async def test_gatt_write_timeout_identifies_actual_stage(self):
        class Hung:
            async def write_gatt_char(self,*args,**kw):await asyncio.sleep(2)
        with self.assertRaisesRegex(RuntimeError,'GATT 控制写入超时'):
            await self.a._write(Hung(),b'','控制')

    async def test_unchanged_gains_are_sent_once_per_session(self):
        await asyncio.sleep(.6)
        self.assertEqual(sum(d[2]==3 for d in self.a.client.writes),2)

    async def test_live_gain_change_requires_stop(self):
        await self.enable()
        with self.assertRaisesRegex(ValueError,'先停止'):self.a.command('gains',dict(kp=40,kd=1.5))
        self.assertEqual(self.a.kp,60)

    async def test_stop_restores_configured_gains_once_before_next_enable(self):
        await self.release();self.a.command('gains',dict(kp=20,kd=.5))
        await self.until(lambda:not self.a.pending and not self.a.gains_dirty)
        before=sum(d[2]==3 for d in self.a.client.writes)
        self.a.stop('下游复位后停止')
        await self.until(lambda:not self.a.gains_dirty)
        gain_frames=[d for d in self.a.client.writes if d[2]==3]
        self.assertEqual(len(gain_frames),before+1)
        self.assertEqual(struct.unpack_from('<HH',gain_frames[-1],4),(200,50))

    async def test_slow_response_does_not_starve_control_with_redundant_gain_writes(self):
        await self.release();self.a.client.write_delay=.13
        self.a.command('enable');await self.until(lambda:self.a.arm_confirmed and not self.a.pending)
        end=time.monotonic()+1.1
        while time.monotonic()<end:
            self.a.alive(True);await asyncio.sleep(.02)
            self.assertTrue(self.a.en,self.a.snapshot())
        self.assertLess(self.a.control_gap_ms,250)
        self.assertEqual(sum(d[2]==3 for d in self.a.client.writes),2)

    async def test_parallel_program_uses_new_ble_target_without_blocking_x(self):
        from unified import UnifiedBridge
        from protocol import parse_status
        from test_ramp import status as x_status
        class X:
            def __init__(self):self.seq=0
            def call(self,name,*args):
                if name=='move':
                    self.seq+=1;op,speed=args[:2]
                    b._events.put(('status',parse_status(x_status(self.seq,op,speed,1,100,op))))
                    b._events.put(('available',True))
                elif name in ('stop','soft_stop'):
                    self.seq+=1
                    b._events.put(('status',parse_status(x_status(self.seq))))
                    b._events.put(('available',False));b._events.put(('available',True))
        x=X();b=UnifiedBridge(x,self.a)
        b._events.put(('status',parse_status(x_status(0))));b._events.put(('available',True));b.refresh(True)
        b.program_edit('x','append',dict(kind='x',seconds=2,direction=1,speed=40,accel=200,decel=200))
        b.program_edit('target','append',dict(kind='target',seconds=.2,yaw_rate=10,pitch_rate=0))
        b.program_start_all({key:p.generation for key,p in b._programs.items()})
        end=time.monotonic()+3;overlap=False
        while time.monotonic()<end:
            snap=b.refresh(True)
            if all(snap['programs'][k]['phase']=='running' for k in ('x','target')):overlap=True
            if snap['programs']['target']['phase']=='complete':break
            await asyncio.sleep(.02)
        self.assertTrue(overlap)
        self.assertEqual(b._programs['target'].phase,'complete')
        self.assertTrue(b._programs['x'].active)
        b.stop_all()

    async def test_first_gain_frame_is_zero_and_enable_captures_latest_pose(self):
        first=next(d for d in self.a.client.writes if d[2]==3)
        self.assertEqual(struct.unpack_from('<HH',first,4),(0,0))
        await self.release()
        with self.assertRaisesRegex(ValueError,'先使能'):self.a.command('angles',dict(yaw=120,pitch=0))
        self.a.client.position=-25
        await self.until(lambda:self.a.snapshot()['status']['st_yaw']==-25)
        self.a.command('enable');await self.until(lambda:self.a.arm_confirmed)
        self.assertAlmostEqual(self.a.last_target[0],-25,places=1)
        self.assertFalse(self.a.planner.velocity);self.assertFalse(self.a.planner.cruise)

    async def test_config_mismatch_blocks_enable_and_old_register_reply_cannot_unlock(self):
        await self.release();self.a.parameters.config_verified=False
        with self.assertRaisesRegex(ValueError,'读取电机配置'):self.a.command('enable')
        self.a._receive(self.a.connection_token,frame(5,1,bytes([22,1])+struct.pack('<f',30)+bytes(5)))
        self.assertFalse(self.a.parameters.config_verified)

    async def test_capability_readback_replaces_manual_flash_assertion(self):
        await self.release();self.a.firmware_alignment_confirmed=False
        self.a.command('enable');await self.until(lambda:self.a.arm_confirmed)
        self.assertTrue(self.a.en)

    async def test_unconfigured_gains_block_enable_even_after_release(self):
        await self.release();self.a.gains_configured=False
        with self.assertRaisesRegex(ValueError,'明确应用'):self.a.command('enable')

    async def test_actual_position_above_180_can_enable_without_clamp(self):
        await self.release();self.a.client.position=200
        await self.until(lambda:self.a.snapshot()['status']['st_yaw']==200)
        self.a.command('enable');await self.until(lambda:self.a.arm_confirmed)
        self.assertEqual(self.a.planner.yaw,200)

    async def test_telemetry_flood_keeps_bounded_samples_and_stop_authority(self):
        await self.release()
        for i in range(1000):
            self.a.client.seq=(self.a.client.seq+1)&255
            self.a._receive(self.a.connection_token,status_frame(self.a.client.seq,0,0))
            if i==500:self.a.stop('高频遥测期间停止')
            if i%50==0:await asyncio.sleep(0)
        await self.until(lambda:self.a.snapshot()['controller_stop_confirmed'])
        await asyncio.sleep(.1)
        self.assertLessEqual(len(self.a.samples),320)
        self.assertTrue(self.a.locked);self.assertFalse(self.a.en)
        self.assertTrue(all(not struct.unpack_from('<H',d,4)[0]&1 for d in self.a.client.writes if d[2]==1))

    async def test_parameter_write_requires_explicit_readback_and_stop_cancels_reads(self):
        self.a.command('write_parameter',dict(rid=6,dtype='float',value=5))
        await self.until(lambda:not self.a.pending)
        self.assertIn('回读一致',self.a.parameters.result)
        writes=[d for d in self.a.client.writes if d[2]==4 and d[5]==6]
        self.assertEqual([d[4] for d in writes],[3,1])
        self.a.command('read_parameter',dict(rid=6,dtype='float'));self.a.stop()
        self.assertFalse(self.a.parameters.busy)


class ShutdownTests(unittest.TestCase):
    def feedback_age_adapter(self,now,health_at,stm_at,motor_at):
        a=BleTargetAdapter(background=False,clock=lambda:now)
        a.connected=True;a.health_at=health_at;a.stm_at=stm_at;a.motor_at=motor_at
        a.health=dict(flags=11,age=20,motor_age=20)
        a.stm=dict(state=0,error=0);a.motor=dict(safety=0,vel=0)
        return a

    def test_new_downstream_feedback_supersedes_old_health_transit_age(self):
        a=self.feedback_age_adapter(.32,0,.28,.28);s=a._status()
        self.assertEqual(s['st_age'],60);self.assertEqual(s['motor_age'],60)
        self.assertTrue(s['st_fresh']);self.assertTrue(s['motor_fresh']);self.assertEqual(s['safe'],0)

    def test_fresh_health_cannot_revive_old_stm_or_motor_frames(self):
        a=self.feedback_age_adapter(.4,.39,0,0);s=a._status()
        self.assertEqual(s['st_age'],400);self.assertEqual(s['motor_age'],400)
        self.assertFalse(s['st_fresh']);self.assertFalse(s['motor_fresh']);self.assertEqual(s['safe'],1)

    def test_bridge_source_age_is_preserved_even_with_new_notifications(self):
        a=self.feedback_age_adapter(.32,0,.31,.31)
        a.health.update(age=310,motor_age=310);s=a._status()
        self.assertEqual(s['st_age'],320);self.assertEqual(s['motor_age'],320)
        self.assertFalse(s['st_fresh']);self.assertFalse(s['motor_fresh'])

    def test_cancel_connect_clears_endpoint_and_allows_another_search(self):
        import threading
        entered=threading.Event();cleaned=threading.Event()
        class Waiting:
            def __init__(self,*args,**kw):pass
            async def connect(self):entered.set();await asyncio.sleep(60)
            async def disconnect(self):cleaned.set()
        adapter=BleTargetAdapter(client_factory=Waiting)
        adapter.devices['sim']=None;adapter.connect('sim')
        try:
            self.assertTrue(entered.wait(2))
            adapter.command('disconnect')
            self.assertTrue(cleaned.wait(2))
            # Client cleanup signals before the owner's finally releases the
            # endpoint reservation. Await that public completion separately.
            end=time.monotonic()+2
            while adapter.endpoint is not None and time.monotonic()<end:time.sleep(.01)
            self.assertIsNone(adapter.endpoint)
            self.assertFalse(adapter.en)
            self.assertTrue(adapter.locked)
        finally:adapter.close()

    def test_close_sends_stop_and_releases_connected_client(self):
        adapter=BleTargetAdapter(client_factory=SimTarget)
        adapter.devices['sim']=None
        adapter.connect('sim')
        try:
            end=time.monotonic()+3
            while not adapter.connected and time.monotonic()<end:time.sleep(.02)
            self.assertTrue(adapter.connected)
            client=adapter.client
            adapter.close()
            self.assertFalse(client.is_connected)
            self.assertFalse(adapter.thread.is_alive())
            self.assertTrue(adapter.loop.is_closed())
            self.assertTrue(adapter.operation.done())
            self.assertTrue(any(d[2]==1 and struct.unpack_from('<H',d,4)[0]&8 for d in client.writes))
        finally:adapter.close()

    def test_close_cancels_inflight_scan_and_closes_owner(self):
        entered=__import__('threading').Event()
        finished=__import__('threading').Event()
        class Scanner:
            @staticmethod
            async def discover(**kw):
                entered.set()
                try:await asyncio.sleep(60)
                finally:finished.set()
        adapter=BleTargetAdapter(scanner=Scanner)
        adapter.scan()
        try:
            self.assertTrue(entered.wait(2))
            adapter.close()
            self.assertTrue(finished.is_set())
            self.assertFalse(adapter.thread.is_alive())
            self.assertTrue(adapter.loop.is_closed())
        finally:adapter.close()


class ConnectTests(unittest.IsolatedAsyncioTestCase):
    async def test_reconnect_exceeds_four_attempts_without_old_enable_replay(self):
        from unittest.mock import patch
        clients=[];real_sleep=asyncio.sleep
        class Flaky(SimTarget):
            def __init__(self,*args,**kw):
                super().__init__(*args,**kw);clients.append(self);self.fail=len(clients)<7
        async def quick_backoff(seconds):await real_sleep(.001 if seconds>=.5 else seconds)
        a=BleTargetAdapter(client_factory=Flaky,background=False);a.en=True
        with patch('target_ble.asyncio.sleep',quick_backoff):
            task=asyncio.create_task(a._connect_loop(None))
            try:
                end=time.monotonic()+3
                while (len(clients)<7 or not clients[-1].writes) and time.monotonic()<end:await real_sleep(.01)
                self.assertGreaterEqual(len(clients),7);self.assertFalse(a.en);self.assertTrue(a.locked)
                self.assertFalse(a.parameters.config_verified);self.assertFalse(a.gains_configured)
                self.assertTrue(all(not struct.unpack_from('<H',d,4)[0]&1 for c in clients for d in c.writes if d[2]==1))
            finally:a.closed=True;task.cancel();await asyncio.gather(task,return_exceptions=True)

    async def test_retry_cleans_up_and_reports_connection_stage(self):
        clients=[];reports=[]
        class Never:
            def __init__(self,*a,**kw):self.kw=kw;self.disconnects=0;clients.append(self)
            async def connect(self):await asyncio.sleep(1)
            async def disconnect(self):self.disconnects+=1
        with self.assertRaisesRegex(TimeoutError,'尚未进入 STM32'):
            await open_ble(Never,None,SERVICE,reports.append,timeout=.01)
        self.assertEqual(len(clients),2);self.assertTrue(all(c.disconnects==1 for c in clients))
        self.assertNotIn('services',clients[0].kw)
    async def test_wrong_service_is_not_retried_as_a_motion_device(self):
        class Wrong:
            def __init__(self,*a,**kw):self.services=type('S',(),{'get_service':lambda s,x:None})()
            async def connect(self):pass
            async def disconnect(self):pass
        with self.assertRaisesRegex(RuntimeError,'UUID'):
            await open_ble(Wrong,None,SERVICE,lambda x:None,timeout=.01)

    async def test_uuid_query_failure_is_avoided_and_default_policy_can_recover(self):
        from unittest.mock import patch
        clients=[]
        class Adapter:
            def __init__(self,*args,**kw):
                self.kw=kw;self.services=Services();self.disconnected=False;clients.append(self)
            async def connect(self):
                if 'services' in self.kw:raise AssertionError('UUID-filtered WinRT path must not be used')
                if 'winrt' in self.kw:
                    exc=PermissionError('injected unsupported uncached query');exc.winerror=-2147024874;raise exc
            async def disconnect(self):self.disconnected=True
        with patch('ble_connect.sys.platform','win32'):
            client=await open_ble(Adapter,None,SERVICE,lambda x:None,timeout=.1)
        self.assertIs(client,clients[1]);self.assertTrue(clients[0].disconnected)
        self.assertNotIn('winrt',clients[1].kw)

    async def test_cancelling_connect_releases_radio_lock_for_other_device(self):
        from ble_connect import radio_setup
        entered=asyncio.Event();cleaned=asyncio.Event()
        class Waiting:
            def __init__(self,*args,**kw):pass
            async def connect(self):entered.set();await asyncio.sleep(60)
            async def disconnect(self):cleaned.set()
        task=asyncio.create_task(open_ble(Waiting,None,SERVICE,lambda x:None))
        await entered.wait();task.cancel()
        with self.assertRaises(asyncio.CancelledError):await task
        self.assertTrue(cleaned.is_set())
        async def acquire():
            async with radio_setup():return True
        self.assertTrue(await asyncio.wait_for(acquire(),.2))

    async def test_named_scan_candidate_still_requires_real_matching_service(self):
        from ble_connect import device_matches
        from types import SimpleNamespace
        dev=SimpleNamespace(name='TC-Target')
        adv=SimpleNamespace(local_name=None,service_uuids=None)
        self.assertTrue(device_matches(dev,adv,SERVICE,('TC-Target',)))
        self.assertFalse(device_matches(dev,adv,SERVICE,('TC-XAxis',)))


if __name__=='__main__':unittest.main()
