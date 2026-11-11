import unittest,math,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from target_wire import control,VELOCITY_FLAG,CAPABILITY_VALUE
from manual_speed_limits import MANUAL_YAW_MAX_DPS
from test_target_ble import AdapterTests as Base
class Codec(unittest.TestCase):
 def test_points_and_units(self):
  for v in [0,90,180,360,-90,-180,-360]:
   p=control(7,VELOCITY_FLAG,0,0,v,0)
   self.assertEqual(struct.unpack_from('<h',p,11)[0],v*10)
   self.assertEqual(len(p),17)
  self.assertAlmostEqual(math.radians(360),2*math.pi)
  self.assertEqual(360/6,60)
 def test_reject_invalid(self):
  for v in [360.1,-360.1,1e9,float('nan'),float('inf'),True,'360']:
   with self.assertRaises(ValueError):control(1,VELOCITY_FLAG,0,0,v,0)
  with self.assertRaises(ValueError):control(1,0,0,0,360,0)
class ManualAdapter(unittest.IsolatedAsyncioTestCase):
 asyncSetUp=Base.asyncSetUp;asyncTearDown=Base.asyncTearDown;until=Base.until;release=Base.release;enable=Base.enable
 async def test_manual_points_and_program_stays_limited(self):
  await self.enable()
  for v in [0,90,180,360,-90,-180,-360]:
   self.a.command('velocity',dict(yaw_rate=v,pitch_rate=0));await self.until(lambda:not self.a.pending)
   await self.until(lambda:any(d[2]==1 and struct.unpack_from('<h',d,11)[0]==v*10 for d in self.a.client.writes))
   self.assertEqual(self.a.planner.yaw_rate,v)
  before=self.a.planner.yaw_rate
  for v in [360.1,-360.1,float('nan'),float('inf'),True,'360']:
   with self.assertRaises(ValueError):self.a.command('velocity',dict(yaw_rate=v,pitch_rate=0))
   self.assertEqual(self.a.planner.yaw_rate,before)
  with self.assertRaises(ValueError):self.a.command('program_velocity',dict(yaw_rate=180,pitch_rate=0))
  self.a.command('stop');await self.until(lambda:not self.a.en)
  self.assertFalse(self.a.planner.velocity)
del Base
