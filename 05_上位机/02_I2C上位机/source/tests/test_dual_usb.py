import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import asyncio,struct,time,unittest,queue,zlib
from usb_transport import UsbClient,wn_frame,Parser,usb_frame
from target_usb import TargetUsbClient
from protocol import STATUS,command
from target_wire import STATUS as TSTATUS,control,gains
from serial_hub import SerialHub
from test_usb import FakeSerial,status_body,wave_body
from test_control import pulse_packets
from test_target_ble import status_frame

def target_body(c,raw=None,boot=8,epoch=1,session=None,result=0,seq=1):
 b=bytearray(wn_frame(3,2,boot,epoch,c.session if session is None else session,seq));b[24:28]=bytes([result,2,5,len(raw) if raw is not None else 20])
 raw=raw if raw is not None else b'TB\x01'+struct.pack('<BIIHHBBH',11,boot,epoch,0,0,seq&255,0,0)
 b[40:40+len(raw)]=raw;struct.pack_into('<I',b,60,zlib.crc32(b[:60]));return bytes(b)

class DualUsbTests(unittest.IsolatedAsyncioTestCase):
 async def asyncSetUp(self):
  self.x=UsbClient('usb:fake-dual',serial_factory=FakeSerial);self.t=TargetUsbClient('usb:fake-dual',serial_factory=FakeSerial)
  await self.x.connect();await self.t.connect();self.x.challenge=(7,1);self.t.challenge=(8,1);self.x.control_ready=self.t.control_ready=True
 async def asyncTearDown(self):
  await self.x.disconnect();await self.t.disconnect();self.assertNotIn('fake-dual',SerialHub.registry)
 async def test_one_COM_two_queues_and_unicast_concurrent_commands(self):
  self.assertIs(self.x.hub,self.t.hub)
  await asyncio.gather(self.x.write_gatt_char(None,command(1,30,7)),self.t.write_gatt_char(None,control(1,8,0,0)))
  frames=[b for k,b in Parser().feed(b''.join(self.x.serial.writes)) if b[3] in (2,5)]
  self.assertEqual({(b[3],struct.unpack_from('<I',b,4)[0]) for b in frames},{(2,1),(5,2)})
  self.assertEqual(len([b for b in frames if b[3]==2]),1)
 async def test_source_isolation_and_two_feedback_pages(self):
  xs=[];ts=[];self.x.callbacks[STATUS]=lambda _,d:xs.append(bytes(d));self.t.callbacks[TSTATUS]=lambda _,d:ts.append(bytes(d))
  self.x.hub.route(2,status_body(self.x));self.x.hub.route(2,target_body(self.t,status_frame(1)))
  await asyncio.sleep(.04);self.assertEqual(len(xs),1);self.assertEqual(len(ts),1);self.assertEqual(ts[0][0],0x5a)
  self.x.receive(2,target_body(self.t));self.t.receive(2,status_body(self.x));self.assertEqual((len(xs),len(ts)),(1,1))
 async def test_wave_flood_bounded_does_not_block_target_parameter_reply(self):
  ts=[];self.t.callbacks[TSTATUS]=lambda _,d:ts.append(bytes(d));b=wave_body(self.x,pulse_packets()[0],0)
  for _ in range(1000):self.x.hub.route(4,b)
  self.assertLessEqual(self.x.mailbox[1].qsize(),8);self.assertEqual(self.t.mailbox[1].qsize(),0)
  self.x.hub.route(2,target_body(self.t));await asyncio.sleep(.04);self.assertTrue(ts);self.assertGreater(self.x.hub.drops,0)
 async def test_one_lane_loss_rotates_only_its_session_and_no_old_motion_replay(self):
  old=self.x.session;ts=self.t.session;self.x.invalidate('simulated single node loss')
  self.assertNotEqual(old,self.x.session);self.assertEqual(ts,self.t.session);self.assertTrue(self.t.control_ready)
  await self.x.write_gatt_char(None,command(1,30,8));await self.t.write_gatt_char(None,control(2,8,0,0))
  frames=[b for _,b in Parser().feed(b''.join(self.x.serial.writes))];self.assertTrue(any(b[3]==1 and b[4]==1 for b in frames));self.assertFalse(any(b[3]==2 for b in frames))
 async def test_disconnect_one_lane_keeps_shared_port_for_other(self):
  serial=self.x.serial;await self.x.disconnect();self.assertFalse(serial.closed)
  await self.t.write_gatt_char(None,control(3,8,0,0));self.assertTrue(serial.writes)
 async def test_bad_target_session_retires_before_control_unlock(self):
  self.t.receive(2,target_body(self.t,session=self.t.session^123));self.assertFalse(self.t.control_ready)
  seen=[];self.t.callbacks[TSTATUS]=lambda _,d:seen.append(d)
  self.t.challenge=(8,2);self.t.retired.append((8,1));self.t.receive(2,target_body(self.t,epoch=1));self.assertEqual(seen,[])
 async def test_target_handshake_drops_motion_and_parameter_before_stop(self):
  self.t.control_ready=False
  await self.t.write_gatt_char(None,gains(1,10,.2));await self.t.write_gatt_char(None,control(2,1,0,0))
  self.assertFalse(any(b[3]==5 for _,b in Parser().feed(b''.join(self.x.serial.writes))))
  await self.t.write_gatt_char(None,control(3,8,0,0));self.assertTrue(any(b[3]==5 for _,b in Parser().feed(b''.join(self.x.serial.writes))))
 async def test_late_USB_mail_is_discarded(self):
  got=[];self.x.callbacks[STATUS]=lambda _,d:got.append(d)
  self.x.mailbox[0].put((time.monotonic()-1,2,status_body(self.x)));await asyncio.sleep(.04);self.assertEqual(got,[])
 async def test_parameter_payload_keeps_target_identity_and_raw_readback(self):
  from target_wire import frame
  request=frame(4,7,bytes([2,22])+struct.pack('<f',45)+bytes(5))
  await self.t.write_gatt_char(None,request)
  packets=[b for _,b in Parser().feed(b''.join(self.x.serial.writes)) if b[3]==5]
  self.assertEqual(packets[-1][24:41],request);self.assertEqual(packets[-1][4],2)
  raw=frame(5,9,bytes([22,1])+struct.pack('<f',45)+bytes(5));received=[]
  self.t.callbacks[TSTATUS]=lambda _,b:received.append(bytes(b));self.x.hub.route(2,target_body(self.t,raw))
  await asyncio.sleep(.04);self.assertEqual(received,[raw])

class AllStopTests(unittest.TestCase):
 def test_offline_and_old_feedback_never_claim_all_stopped(self):
  from unified import UnifiedBridge
  from test_desktop import Recorder
  class Target:
   motor_at=time.monotonic()-1
   def alive(self,*args):pass
   def stop(self,*args):return True
   def snapshot(self):return dict(controller_stop_confirmed=True,generation=1)
  t=Target();b=UnifiedBridge(Recorder(),t);b.stop_all()
  state=b.refresh(False)
  self.assertFalse(state['all_stop']['confirmed']);self.assertEqual(state['all_stop']['target'],'停止未确认')
  self.assertFalse(state['all_stop']['physical_stop_confirmed'])
  t.motor_at=time.monotonic()+.01
  state=b.refresh(False);self.assertFalse(state['all_stop']['confirmed']);self.assertEqual(state['all_stop']['x'],'停止未确认')

 def test_all_stop_requires_new_X_transaction_and_fresh_target_feedback(self):
  from unified import UnifiedBridge
  from test_desktop import Recorder
  from types import SimpleNamespace
  class Target:
   motor_at=None
   def alive(self,*args):pass
   def stop(self,*args):return True
   def snapshot(self):return dict(controller_stop_confirmed=True,generation=1)
  t=Target();c=Recorder();c.channel=SimpleNamespace(stop_seq=5,barrier=False,synced=True)
  b=UnifiedBridge(c,t);b.stop_all()
  b._status_revision+=1;b._state.update(available=True,connected=True,status=dict(online=True,uart=True,demo=False,flags=185,error=0,motion=0,phase=0,current_hz=0,ack=5,stm_ack=5))
  t.motor_at=time.monotonic()+.01
  self.assertFalse(b.refresh(False)['all_stop']['confirmed'])
  c.channel.stop_seq=6;b._state['status'].update(ack=6,stm_ack=6)
  result=b.refresh(False)['all_stop'];self.assertTrue(result['confirmed']);self.assertFalse(result['physical_stop_confirmed'])

if __name__=='__main__':unittest.main()
