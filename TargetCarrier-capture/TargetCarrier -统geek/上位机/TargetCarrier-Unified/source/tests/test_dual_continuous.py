"""6.1 application payloads through the production GEEK node-2 adapter."""
import asyncio,struct,unittest
from test_dual_usb import DualUsbTests,target_body
from usb_transport import Parser
from target_wire import control,frame,STATUS,VELOCITY_FLAG,CAPABILITY_RID,CAPABILITY_VALUE
from target_parameters import TargetParameters

class DualContinuousTests(unittest.IsolatedAsyncioTestCase):
 asyncSetUp=DualUsbTests.asyncSetUp
 asyncTearDown=DualUsbTests.asyncTearDown
 async def test_velocity_and_smooth_stop_keep_flag_and_node(self):
  for seq,speed in [(1,10),(2,0),(3,-10)]:
   raw=control(seq,1|16|VELOCITY_FLAG,0,0,speed,0)
   await self.t.write_gatt_char(None,raw)
   packet=[b for _,b in Parser().feed(b''.join(self.t.serial.writes)) if b[3]==5][-1]
   self.assertEqual(struct.unpack_from('<I',packet,4)[0],2)
   self.assertEqual(packet[24:41],raw)
   self.assertTrue(struct.unpack_from('<H',packet,28)[0]&VELOCITY_FLAG)
 async def test_capability_readback_through_node_two(self):
  params=TargetParameters(lambda:0);params.start('read_config',{})
  from target_wire import decode
  self.t.callbacks[STATUS]=lambda _,data:params.receive(decode(data)[1])
  for rid,value,fmt in [(CAPABILITY_RID,CAPABILITY_VALUE,'I'),(10,1,'I'),(21,12.5,'f'),(22,45.,'f'),(23,10.,'f')]:
   request=params.next_packet(1);await self.t.write_gatt_char(None,request)
   raw=frame(5,1,bytes([rid,1])+struct.pack('<'+fmt,value)+bytes(5))
   self.t.hub.route(2,target_body(self.t,raw));await asyncio.sleep(.04)
  self.assertTrue(params.config_verified)
 async def test_reconnect_drops_continuous_request_until_new_stop_session(self):
  self.t.invalidate('reset')
  await self.t.write_gatt_char(None,control(1,1|16|VELOCITY_FLAG,0,0,10,0))
  self.assertFalse(any(b[3]==5 for _,b in Parser().feed(b''.join(self.t.serial.writes))))

del DualUsbTests
