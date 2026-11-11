import math,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from target_wire import quantized_gains,orientation,control,VELOCITY_FLAG,CAPABILITY_RID,CAPABILITY_VALUE
from target_motion import TargetMotion
from target_parameters import TargetParameters
from test_target_ble import AdapterTests as _AdapterTests

class ContinuousCodecTests(unittest.TestCase):
    def test_zero_debug_and_two_stage_gain_quantization(self):
        self.assertEqual(quantized_gains(0,0),(0,0))
        for kp in (.01,.04,.1):
            with self.assertRaises(ValueError):quantized_gains(kp,.03)
        self.assertEqual(quantized_gains(.2,.03),(.2,.03))
        for value in (float('nan'),float('inf'),True):
            with self.assertRaises(ValueError):quantized_gains(value,0)
    def test_orientation_is_not_a_travel_clamp(self):
        for angle in (-100000,200,360,720,100000):
            yaw=orientation(angle)
            self.assertTrue(-180<=yaw<180)
            self.assertAlmostEqual((angle-yaw)%360,0)
            self.assertEqual(len(control(1,VELOCITY_FLAG,yaw,0,10)),17)
    def test_twelve_hour_pc_speed_does_not_accumulate_position(self):
        m=TargetMotion();m.hold(650,0);m.rates(10,0)
        for _ in range(864000):self.assertEqual(m.step(.05),(650,0,10,0))
        m.hold(651,0);self.assertFalse(m.velocity);self.assertEqual(m.step(.05)[2],0)
    def test_scan_starts_at_current_pose_and_has_no_half_second_dwell(self):
        m=TargetMotion();m.hold(65,0);m.sweep_amp=20;m.sweep_speed=10;m.select(2)
        prev=65;values=[]
        for _ in range(1000):
            y,p,v,_=m.step(.05);values.append(y)
            self.assertLessEqual(abs(y-prev),10*.05+1e-7);self.assertTrue(45<=y<=85)
            prev=y
        self.assertGreater(max(values),84);self.assertLess(min(values),46)
        self.assertLess(abs(values[0]-65),.1)
        self.assertFalse(any(values[i:i+10]==[values[i]]*10 for i in range(len(values)-10)))
    def test_old_firmware_cannot_confirm_configuration(self):
        now=[0.];p=TargetParameters(lambda:now[0]);p.start('read_config',{})
        self.assertEqual(p.next_packet(1)[5],CAPABILITY_RID)
        now[0]=1.1;p.next_packet(2);now[0]=2.2;p.next_packet(3)
        self.assertFalse(p.config_verified);self.assertIn('配套固件',p.config_result)
    def test_wrong_capability_and_control_mode_block_enable(self):
        for cap,mode in ((0,1),(CAPABILITY_VALUE,2)):
            p=TargetParameters(lambda:0);p.start('read_config',{})
            for rid,v,kind in ((254,cap,'I'),(10,mode,'I'),(21,12.5,'f'),(22,45.,'f'),(23,10.,'f')):
                p.next_packet(1);p.receive(dict(rid=rid,ok=1,raw=struct.pack('<'+kind,v).hex()))
            self.assertFalse(p.config_verified)

class ContinuousAdapterTests(unittest.IsolatedAsyncioTestCase):
    asyncSetUp=_AdapterTests.asyncSetUp
    asyncTearDown=_AdapterTests.asyncTearDown
    until=_AdapterTests.until
    release=_AdapterTests.release
    enable=_AdapterTests.enable
    async def test_zero_gains_enable_is_allowed_but_rotation_rejected(self):
        self.a.command('gains',dict(kp=0,kd=0));await self.until(lambda:not self.a.pending and not self.a.gains_dirty)
        await self.enable();self.assertTrue(self.a.en)
        with self.assertRaisesRegex(ValueError,'全零'):self.a.command('velocity',dict(yaw_rate=10,pitch_rate=0))
        with self.assertRaisesRegex(ValueError,'非零'):self.a.command('angles',dict(yaw=10,pitch=0))
    async def test_speed_frame_has_distinct_flag_and_smooth_stop_does_not_disarm(self):
        self.a.command('gains',dict(kp=0,kd=.07));await self.until(lambda:not self.a.pending and not self.a.gains_dirty)
        await self.enable();self.a.command('velocity',dict(yaw_rate=10,pitch_rate=0));await self.until(lambda:not self.a.pending)
        packets=[d for d in self.a.client.writes if d[2]==1]
        self.assertTrue(struct.unpack_from('<H',packets[-1],4)[0]&VELOCITY_FLAG)
        self.assertEqual(struct.unpack_from('<h',packets[-1],11)[0],100)
        self.a.command('smooth_stop');await self.until(lambda:not self.a.pending)
        self.assertTrue(self.a.en);self.assertTrue(self.a.planner.velocity);self.assertEqual(self.a.planner.yaw_rate,0)
    async def test_pre_enable_motion_rejected_without_remembering_old_command(self):
        await self.release()
        for name,args in [('velocity',dict(yaw_rate=10,pitch_rate=0)),('profile',dict(profile=2)),('cruise',dict(speed=10))]:
            with self.assertRaisesRegex(ValueError,'先使能'):self.a.command(name,args)
        self.assertFalse(self.a.planner.velocity);self.assertFalse(self.a.en)
    async def test_gain_quantization_rejection_preserves_old_values(self):
        before=(self.a.kp,self.a.kd)
        with self.assertRaises(ValueError):self.a.command('gains',dict(kp=.01,kd=.03))
        self.assertEqual(before,(self.a.kp,self.a.kd))

del _AdapterTests
