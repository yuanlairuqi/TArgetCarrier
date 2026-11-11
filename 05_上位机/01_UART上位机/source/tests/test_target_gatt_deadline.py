
import asyncio,sys,time,unittest
from pathlib import Path
from types import SimpleNamespace
sys.path[:0]=[str(Path(__file__).resolve().parents[1]),str(Path(__file__).resolve().parent)]
from target_ble import BleTargetAdapter
from ble_connect import open_ble
from bounded_gatt import cleanup_pending
from bleak.backends.winrt.client import FutureLike,AsyncStatus
from test_target_ble import SimTarget
from target_wire import SERVICE
class Native:
 def __init__(self):self.status=AsyncStatus.STARTED;self.completed=None;self.cancels=0
 def cancel(self):self.cancels+=1
 def get_results(self):return None
 def finish(self):
  if self.status==AsyncStatus.STARTED:
   self.status=AsyncStatus.CANCELED
   if self.completed:self.completed(self,self.status)
class Hanging(SimTarget):
 instances=[]
 def __init__(self,*a,**kw):super().__init__(*a,**kw);self.ops=[];Hanging.instances.append(self)
 async def write_gatt_char(self,*a,**kw):
  n=Native();self.ops.append(n);await FutureLike(n)
 async def disconnect(self):
  n=Native();self.ops.append(n)
  try:await FutureLike(n)
  finally:
   self.is_connected=False
   if hasattr(self,'task'):self.task.cancel()
 async def release(self):
  for n in self.ops:n.finish()
  await asyncio.sleep(.05)
  if hasattr(self,'task'):await asyncio.gather(self.task,return_exceptions=True)
class Tests(unittest.IsolatedAsyncioTestCase):
 async def test_target_write_has_independent_deadline_and_fences_reconnect(self):
  a=BleTargetAdapter(background=False);c=Hanging();c.is_connected=True
  started=time.monotonic()
  try:
   with self.assertRaises(RuntimeError):await a._write(c,b'packet','test')
   self.assertLess(time.monotonic()-started,.9);self.assertTrue(cleanup_pending(a))
   a.devices={'x':None}
   with self.assertRaises(ValueError):a.connect('x')
  finally:await c.release()
  self.assertFalse(cleanup_pending(a))
 async def test_session_timeout_cleanup_bounded_and_late_notifications_ignored(self):
  Hanging.instances.clear();a=BleTargetAdapter(client_factory=Hanging,background=False);started=time.monotonic()
  try:
   with self.assertRaises(RuntimeError):await a._session('test')
   self.assertLess(time.monotonic()-started,3.2)
   c=Hanging.instances[0];self.assertTrue(cleanup_pending(a));self.assertFalse(a.en);self.assertFalse(a.connected);self.assertIsNone(a.connection_token)
   before=dict(a.rx_counts);c.callback(None,c.health());self.assertEqual(a.rx_counts,before)
   a.devices={'x':None}
   with self.assertRaises(ValueError):a.connect('x')
  finally:
   for c in Hanging.instances:await c.release()
 async def test_connect_timeout_does_not_retry_over_unreleased_native_operation(self):
  class ConnectHang(Hanging):
   async def connect(self):
    self.is_connected=True;n=Native();self.ops.append(n);await FutureLike(n)
  Hanging.instances.clear();owner=SimpleNamespace();started=time.monotonic()
  try:
   with self.assertRaises(RuntimeError):await open_ble(ConnectHang,'test',SERVICE,lambda s:None,timeout=.03,owner=owner)
   self.assertLess(time.monotonic()-started,2.8);self.assertEqual(len(Hanging.instances),1);self.assertTrue(cleanup_pending(owner))
   with self.assertRaises(RuntimeError):await open_ble(ConnectHang,'test',SERVICE,lambda s:None,timeout=.03,owner=owner)
   self.assertEqual(len(Hanging.instances),1)
  finally:
   for c in Hanging.instances:await c.release()

 def test_ui_close_bounded_while_daemon_keeps_unreleased_client_loop_alive(self):
  a=BleTargetAdapter(background=True);c=SimpleNamespace(is_connected=True)
  a._gatt_unreleased_clients=[c];started=time.monotonic()
  try:
   a.close();self.assertLess(time.monotonic()-started,3.7)
   self.assertTrue(a.thread.is_alive());self.assertFalse(a.loop.is_closed())
  finally:
   c.is_connected=False;a.thread.join(timeout=2)
  self.assertFalse(a.thread.is_alive());self.assertTrue(a.loop.is_closed())
