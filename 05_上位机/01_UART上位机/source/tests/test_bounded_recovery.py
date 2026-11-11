
import asyncio,queue,struct,sys,time,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
import controller
from controller import Controller
from bounded_gatt import bounded_await,cleanup_pending,WriteChannelTimeout
from usb_transport import CombinedController
from test_ramp import ProfileClient,status
from bleak.backends.winrt.client import FutureLike,AsyncStatus

class Op:
 def __init__(self,responsive=False):self.status=AsyncStatus.STARTED;self.completed=None;self.cancel_count=0;self.responsive=responsive
 def cancel(self):
  self.cancel_count+=1
  if self.responsive:self.finish()
 def finish(self,success=False):
  if self.status==AsyncStatus.STARTED:
   self.status=AsyncStatus.COMPLETED if success else AsyncStatus.CANCELED
   if self.completed:self.completed(self,self.status)
 def get_results(self):return None

class Client(ProfileClient):
 mode='healthy';instances=[];ack_stop=True
 def __init__(self,*a,**kw):
  super().__init__(*a,**kw);self.mode=type(self).mode;self.native=None;self.cleanup_native=None;self.writes=[];self.moves=0;self.callback=None;type(self).instances.append(self)
 async def start_notify(self,uuid,callback):
  if '0003-' not in uuid:return
  self.callback=callback
  async def updates():
   while self.is_connected:
    callback(None,status(self.seq,self.op,self.speed,2 if self.op else 0,500 if self.op else 0,self.op));await asyncio.sleep(.01)
  self.task=asyncio.create_task(updates())
 async def write_gatt_char(self,tx,data,response=True):
  self.writes.append(bytes(data))
  if data[1]:
   self.moves+=1
   if self.moves==2 and self.mode!='healthy':
    self.native=Op(self.mode=='responsive');await FutureLike(self.native)
  if not data[1] and not type(self).ack_stop:return
  await super().write_gatt_char(tx,data,response)
 async def disconnect(self):
  if self.mode=='cleanup_stuck':self.cleanup_native=Op();await FutureLike(self.cleanup_native)
  if self.mode=='cleanup_error':raise OSError('disconnect refused')
  await super().disconnect()
 async def force_finish(self):
  if self.native:self.native.finish()
  if self.cleanup_native:self.cleanup_native.finish()
  self.is_connected=False
  if hasattr(self,'task'):self.task.cancel();await asyncio.gather(self.task,return_exceptions=True)

class BoundedTests(unittest.IsolatedAsyncioTestCase):
 async def asyncSetUp(self):
  self.saved=controller.open_ble
  async def adapter(factory,device,service,emit,owner=None):
   client=factory(device);await client.connect();return client
  controller.open_ble=adapter;Client.instances=[];Client.ack_stop=True
  self.sessions=[];self.heartbeats=[]
 async def asyncTearDown(self):
  for client in Client.instances:await client.force_finish()
  await asyncio.sleep(.02)
  for t in self.sessions+self.heartbeats:t.cancel()
  await asyncio.gather(*self.sessions,*self.heartbeats,return_exceptions=True)
  controller.open_ble=self.saved
 async def until(self,p,timeout=1.5):
  deadline=time.monotonic()+timeout
  while not p():
   self.assertLess(time.monotonic(),deadline);await asyncio.sleep(.01)
 def owner(self,combined=False):
  c=(CombinedController if combined else Controller).__new__(CombinedController if combined else Controller)
  c.events=queue.Queue(10000);c.client_factory=Client;c.devices={'fake':None};c.client=c.session=None;c.ui_at=time.monotonic();c.ramp=True;c.write_deadline=.12;c.cleanup_deadline=.12
  async def alive():
   while True:c.ui_at=time.monotonic();await asyncio.sleep(.03)
  self.heartbeats.append(asyncio.create_task(alive()));return c
 async def run_stall(self,mode,combined=False):
  Client.mode=mode;c=self.owner(combined);t=asyncio.create_task(c.connect('fake'));self.sessions.append(t)
  await self.until(c.available);client=c.client;c._request('move',(1,50,0,0,0,200,200));await self.until(lambda:client.native is not None)
  started=time.monotonic();await self.until(t.done);elapsed=time.monotonic()-started
  self.assertLess(elapsed,.7);self.assertIsNone(c.held);self.assertFalse(c.channel.synced);self.assertTrue(c.channel.barrier)
  cut=next(i for i,p in enumerate(client.writes) if p[1] and sum(x[1]!=0 for x in client.writes[:i+1])==2)
  self.assertEqual(client.writes[cut+1:],[]) # No concurrent STOP or late movement queued behind native write.
  return c,client,elapsed
 async def test_real_futurelike_three_controls(self):
  Client.mode='healthy';c=self.owner();t=asyncio.create_task(c.connect('fake'));self.sessions.append(t);await self.until(c.available)
  c._request('move',(1,50,0,0,0,200,200));await asyncio.sleep(.55);self.assertTrue(c.available());self.assertFalse(t.done())
  for mode in ['responsive','stuck']:
   c,client,_=await self.run_stall(mode);self.assertGreaterEqual(client.native.cancel_count,1)
 async def test_disconnect_native_cancellation_is_bounded_and_fenced(self):
  c,client,_=await self.run_stall('cleanup_stuck',True);self.assertTrue(cleanup_pending(c));self.assertFalse(c._reconnect_enabled)
  count=len(Client.instances);await c.connect('fake');self.assertEqual(len(Client.instances),count)
  n=c.stats['received'];client.callback(None,status(999,1,50,2,500,1));self.assertEqual(c.stats['received'],n);self.assertIsNone(c.status)
 async def test_manual_reconnect_only_after_release_stop_first_and_old_callbacks_ignored(self):
  c,old,_=await self.run_stall('stuck',True)
  count=len(Client.instances);await c.connect('fake');self.assertEqual(len(Client.instances),count)
  old.native.finish(success=True) # Native write completes late, despite its earlier cancel request.
  await old.force_finish();await self.until(lambda:not cleanup_pending(c))
  Client.mode='healthy';Client.ack_stop=False;t=asyncio.create_task(c.connect('fake'));self.sessions.append(t)
  await self.until(lambda:len(Client.instances)>count and bool(Client.instances[-1].writes));new=Client.instances[-1]
  self.assertEqual(new.writes[0][1],0);self.assertFalse(c.available());self.assertIsNone(c.held)
  seq=c.channel.stop_seq;old.callback(None,status(999,1,50,2,500,1));self.assertNotEqual(c.status.ack,999)
  c._request('move',(1,50,0,0,0,200,200));await asyncio.sleep(.05);self.assertTrue(all(p[1]==0 for p in new.writes))
  Client.ack_stop=True;new.seq=seq;new.op=new.speed=0;await self.until(c.available);self.assertIsNone(c.held);self.assertTrue(all(p[1]==0 for p in new.writes))
 async def test_failed_disconnect_connected_client_remains_fenced_after_write_finishes(self):
  c,client,_=await self.run_stall('cleanup_error');client.native.finish();await asyncio.sleep(.05)
  self.assertTrue(client.is_connected);self.assertTrue(cleanup_pending(c));count=len(Client.instances);await c.connect('fake');self.assertEqual(len(Client.instances),count)
 async def test_bounded_await_outer_cancel_does_not_wait_for_native(self):
  owner=type('Owner',(),{})();op=Op()
  async def native():await FutureLike(op)
  task=asyncio.create_task(bounded_await(owner,native(),5));await asyncio.sleep(.01);task.cancel();done,_=await asyncio.wait([task],timeout=.1);self.assertTrue(done);self.assertTrue(cleanup_pending(owner));op.finish();await self.until(lambda:not cleanup_pending(owner))
