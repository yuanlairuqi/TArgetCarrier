import asyncio
import queue
import struct
import sys
import time
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from protocol import command,parse_status,Channel
from controller import Controller
from usb_transport import wn_frame,usb_frame,Parser

def status(seq,op=0,speed=0,phase=0,hz=0,motion=0,stm=None,mode=0):
    b=bytearray(20);b[:3]=bytes([4,motion,1|8|16|32|128]);struct.pack_into('<H',b,3,hz)
    b[5:8]=bytes([op,speed,0]);struct.pack_into('<IIHBB',b,8,seq,seq if stm is None else stm,1000 if speed else 0,mode,phase)
    return bytes(b)

class RampProtocolTests(unittest.TestCase):
    def test_compact_command_and_gu_unchanged_length(self):
        c=command(1,40,99,1,1000,2000,200,300)
        self.assertEqual(len(c),16)
        self.assertEqual(struct.unpack('<BBBBIHHHH',c),(4,1,40,1,99,10,20,200,300))
        frame=usb_frame(1,wn_frame(2,1,2,3,4,99,c))
        self.assertEqual(len(frame),72);self.assertEqual(Parser().feed(frame)[0][1][24:40],c)
    def test_limits_and_stop_canonicalization(self):
        for a,d in [(0,1),(1,0),(65536,1),(-1,1),(float('nan'),10)]:
            with self.assertRaises(ValueError):command(1,40,1,0,0,0,a,d)
        with self.assertRaises(ValueError):command(1,40,1,1,150,1000,1,1)
        self.assertEqual(command(3,0,1,0,0,0,1,65535)[1],3)
    def test_acceptance_during_accel_does_not_require_cruise(self):
        c=Channel(ramp=True);c.synced=True;c.move(1,40,0,0,0,0,200,200);p=c.take(0);c.writing=None
        c.status(parse_status(status(p.seq,1,40,1,0)))
        self.assertFalse(c.unconfirmed);self.assertIsNone(c.expired(1.2))
    def test_stop_barrier_cannot_unlock_on_decel_or_wrong_ack(self):
        c=Channel(ramp=True);c.stop(0);p=c.take(0);c.writing=None
        c.status(parse_status(status(p.seq,0,0,3,500,1)));self.assertTrue(c.barrier)
        c.status(parse_status(status(p.seq,stm=999)));self.assertTrue(c.barrier)
        c.status(parse_status(status(p.seq)));self.assertFalse(c.barrier)
    def test_v3_does_not_unlock_v4_negotiation(self):
        c=Channel(ramp=True);c.stop(0);p=c.take(0);c.writing=None
        b=bytearray(status(p.seq));b[0]=3;c.status(parse_status(b));self.assertTrue(c.barrier)

class Services:
    def get_service(self,_):return self
    def get_characteristic(self,_):return type('Char',(),{'properties':['write','notify']})()

class StopRaceTests(unittest.TestCase):
    def test_owner_rejects_a_queued_parameter_update_after_soft_stop(self):
        c=Controller.__new__(Controller);c.available=lambda:True;c.ramp=True;c.soft_stopping=False
        c.held=(1,40,0,0,0,200,100)
        c._request('soft_stop',())
        c._request('move',(1,80,0,0,0,300,100))
        self.assertEqual(c.held,(3,0,0,0,0,200,100))
        self.assertTrue(c.soft_stopping)

    def test_bridge_locks_updates_before_owner_consumes_soft_stop(self):
        from desktop import Bridge
        class Fake:
            soft_stopping=False
            def __init__(self):self.calls=[]
            def call(self,*a):self.calls.append(a)
        fake=Fake();b=Bridge(controller=fake);b._state.update(running=True,available=True)
        b._last_focus=time.monotonic();b._run_recipe=(1,0,0,0)
        self.assertTrue(b.action('soft_stop'))
        with self.assertRaises(ValueError):b.update_profile(80,300,100)
        self.assertEqual(fake.calls,[('soft_stop',)])
class ProfileClient:
    def __init__(self,*a,**kw):
        self.is_connected=False;self.services=Services();self.seq=self.op=self.speed=0;self.soft_at=None
    async def connect(self):self.is_connected=True
    async def disconnect(self):self.is_connected=False;self.task.cancel();await asyncio.gather(self.task,return_exceptions=True)
    async def start_notify(self,uuid,callback):
        if '0003-' not in uuid:return
        async def loop():
            while self.is_connected:
                if self.op==3:
                    elapsed=time.monotonic()-self.soft_at
                    callback(None,status(self.seq,3,0,3 if elapsed<1.4 else 0,100 if elapsed<1.4 else 0,1 if elapsed<1.4 else 0))
                elif self.op:callback(None,status(self.seq,self.op,self.speed,1,100,self.op))
                else:callback(None,status(self.seq))
                await asyncio.sleep(.02)
        self.task=asyncio.create_task(loop())
    async def write_gatt_char(self,tx,data,response=True):
        self.seq=struct.unpack_from('<I',data,4)[0];self.op=data[1];self.speed=data[2]
        if self.op==3 and self.soft_at is None:self.soft_at=time.monotonic()

class RampSessionTests(unittest.IsolatedAsyncioTestCase):
    async def test_long_soft_stop_keeps_heartbeats_and_finishes_without_false_timeout(self):
        c=Controller.__new__(Controller);c.events=queue.Queue(10000);c.client_factory=ProfileClient;c.devices={'fake':None}
        c.client=c.session=None;c.ui_at=time.monotonic();c.ramp=True
        task=asyncio.create_task(c.connect('fake'))
        async def keep_ui():
            while True:c.ui_at=time.monotonic();await asyncio.sleep(.08)
        alive=asyncio.create_task(keep_ui())
        try:
            await asyncio.sleep(.2);self.assertTrue(c.channel.synced)
            c.held=(1,40,0,0,0,200,100);await asyncio.sleep(.25)
            c._request('soft_stop',());await asyncio.sleep(1.1)
            self.assertTrue(c.soft_stopping);self.assertFalse(c.channel.timed_out)
            await asyncio.sleep(.6);self.assertFalse(c.soft_stopping);self.assertIsNone(c.held)
            self.assertTrue(c.channel.synced);self.assertEqual(c.client.op,0)
        finally:
            task.cancel();alive.cancel();await asyncio.gather(task,alive,return_exceptions=True)

if __name__=='__main__':unittest.main()
