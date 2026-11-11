import asyncio,queue,struct,time,unittest,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from protocol import Channel,parse_status
from controller import Controller
from usb_transport import CombinedController
from test_ramp import ProfileClient,status

def stopped_fault(seq,error=7,flags=0xB9):
    b=bytearray(status(seq));b[2]=flags;b[7]=error;return bytes(b)

class FaultStopTests(unittest.TestCase):
    def test_current_stopped_ack_with_fault_never_confirms_or_authorizes_motion(self):
        c=Channel(ramp=True);c.stop(0);p=c.take(0);c.writing=None
        s=parse_status(stopped_fault(p.seq));c.status(s)
        self.assertTrue(c.barrier);self.assertFalse(s.stop_feedback);self.assertFalse(s.can_move)
    def test_stale_ack_running_phase_and_peer_fault_never_confirm_stop(self):
        for kind in ['stale','phase','hz','target','peer']:
            with self.subTest(kind=kind):
                c=Channel(ramp=True);c.stop(0);p=c.take(0);c.writing=None;b=bytearray(stopped_fault(p.seq))
                if kind=='stale':struct.pack_into('<I',b,12,p.seq-1)
                elif kind=='phase':b[19]=3
                elif kind=='hz':struct.pack_into('<H',b,3,1)
                elif kind=='target':struct.pack_into('<H',b,16,1)
                else:b[2]|=64
                c.status(parse_status(b));self.assertTrue(c.barrier)
    def test_combined_owner_retries_stop_when_barrier_transaction_was_lost(self):
        c=CombinedController.__new__(CombinedController);c.events=queue.Queue();c.client=type('Client',(),{'is_connected':True})();c.ramp=True
        c.status=parse_status(stopped_fault(1));c.channel=Channel(ramp=True);c.channel.synced=True;c.channel.barrier=True
        c.held=None;c.last_stop=0;c.last_status=2;c.soft_stopping=False;c.motion_generation=0
        c.idle_maintenance(2);self.assertTrue(c.channel.barrier);self.assertEqual(c.channel.pending.motion,0)
        self.assertIsNone(c.held)

class RecoveringClient(ProfileClient):
    def __init__(self,*a,**kw):super().__init__(*a,**kw);self.fault=False;self.stop_writes=0;self.writes=[]
    async def start_notify(self,uuid,callback):
        if '0003-' not in uuid:return
        async def loop():
            while self.is_connected:
                b=bytearray(status(self.seq,self.op,self.speed,0 if not self.op else 2,0 if not self.op else 500,self.op));b[7]=7 if self.fault else 0
                callback(None,b);await asyncio.sleep(.02)
        self.task=asyncio.create_task(loop())
    async def write_gatt_char(self,tx,data,response=True):
        self.writes.append(bytes(data));await super().write_gatt_char(tx,data,response)
        if self.fault and data[1]==0:
            self.stop_writes+=1
            if self.stop_writes>=2:self.fault=False

class FaultSessionTests(unittest.IsolatedAsyncioTestCase):
    async def test_transient_fault_recovers_without_reconnect_and_never_replays_motion(self):
        c=Controller.__new__(Controller);c.events=queue.Queue(5000);c.client_factory=RecoveringClient;c.devices={'fake':None};c.client=c.session=None;c.ui_at=time.monotonic();c.ramp=True
        task=asyncio.create_task(c.connect('fake'))
        async def alive():
            while True:c.ui_at=time.monotonic();await asyncio.sleep(.05)
        heart=asyncio.create_task(alive())
        async def until(f,timeout=4):
            end=time.monotonic()+timeout
            while not f():
                if task.done():await task;self.fail('session ended')
                if time.monotonic()>end:self.fail('transition timed out')
                await asyncio.sleep(.02)
        try:
            await until(c.available);c._request('move',(1,50,0,0,0,200,200));await until(lambda:any(p[1]==1 for p in c.client.writes))
            cut=len(c.client.writes);c.client.fault=True
            await until(lambda:c.held is None);await until(lambda:c.client.stop_writes>=2)
            await until(c.available);self.assertTrue(c.client.is_connected);self.assertIsNone(c.held)
            self.assertTrue(all(p[1]==0 for p in c.client.writes[cut:]))
        finally:task.cancel();heart.cancel();await asyncio.gather(task,heart,return_exceptions=True)
