"""Fault-injection tests. No physical serial, radio or motor is accessed."""
import asyncio,queue,struct,time,unittest,zlib
from unittest.mock import patch
from usb_transport import UsbClient,CombinedController,Parser,usb_frame,wn_frame
from controller import Controller
from protocol import STATUS,PULSE,command
from test_usb import status_body,FakeSerial,RadioSerial,wave_body
from test_control import pulse_packets
from test_desktop import Recorder
from desktop import Bridge

class RecoveryFrames(unittest.TestCase):
    def test_delayed_epoch_cannot_roll_back_new_challenge(self):
        c=UsbClient('usb:fake');c.challenge=(7,1)
        c.receive(2,status_body(c,epoch=2));self.assertEqual(c.challenge,(7,2))
        session=c.session;c.receive(2,status_body(c,epoch=1))
        self.assertEqual(c.challenge,(7,2));self.assertEqual(c.session,session)
    def test_retired_boot_cannot_replace_a_new_boot(self):
        c=UsbClient('usb:fake');c.challenge=(7,1)
        c.receive(2,status_body(c,boot=9));s=c.session
        c.receive(2,status_body(c,boot=7));self.assertEqual(c.challenge,(9,1));self.assertEqual(c.session,s)
    def test_loss_rotates_session_and_does_not_claim_stopped(self):
        c=UsbClient('usb:fake');c.challenge=(7,1);c.control_ready=True;s=c.session
        losses=[];c.on_link_loss=losses.append;c.invalidate('故障注入')
        self.assertIsNone(c.challenge);self.assertFalse(c.control_ready);self.assertNotEqual(c.session,s)
        self.assertTrue(c.need_query);self.assertEqual(losses,['故障注入'])
        b=Bridge(Recorder());b._state['running']=True;b._events.put(('transport_lost','无线丢失'))
        r=b.refresh();self.assertIsNone(r['status']);self.assertFalse(r['available']);self.assertIn('未知',r['hint'])
    def test_master_reset_and_radio_rebuild_revoke_session(self):
        c=UsbClient('usb:fake');b=bytearray(64);b[0]=1;b[63]=1
        struct.pack_into('<I',b,44,1);c.receive(5,b)
        c.challenge=(7,1);c.control_ready=True
        struct.pack_into('<H',b,56,1);c.receive(5,b);self.assertIsNone(c.challenge)
        c.challenge=(7,1);struct.pack_into('<I',b,44,2);c.receive(5,b);self.assertIsNone(c.challenge)
    def test_transport_loss_clears_motion_and_ack_barrier(self):
        c=Controller.__new__(Controller);c.events=queue.Queue();c.client=type('Client',(),dict(is_connected=True))()
        from protocol import Channel
        c.channel=Channel(ramp=True);c.channel.synced=True;c.channel.move(1,40,time.monotonic());c.channel.take(time.monotonic())
        c.held=(1,40);c.status=object();c.soft_stopping=False;c.motion_generation=1
        c.transport_loss('无线无状态')
        self.assertIsNone(c.held);self.assertIsNone(c.status);self.assertFalse(c.channel.synced)
        self.assertTrue(c.channel.barrier);self.assertEqual(c.channel.pending.motion,0)
        self.assertTrue(all(p.motion==0 for p in c.channel.unconfirmed));self.assertGreater(c.motion_generation,1)

class Backpressure(unittest.IsolatedAsyncioTestCase):
    async def test_write_waiting_on_lock_is_discarded_after_loss(self):
        c=UsbClient('usb:fake',serial_factory=FakeSerial);await c.connect()
        try:
            c.challenge=(7,1);c.last_seen=c.last_radio=time.monotonic()
            await c.tx_lock.acquire()
            task=asyncio.create_task(c.write_gatt_char(None,command(1,40,10)))
            await asyncio.sleep(.01);c.invalidate('断联');c.tx_lock.release();await task
            moves=[b for _,b in Parser().feed(b''.join(c.serial.writes)) if b[3]==2 and b[25]!=0]
            self.assertEqual(moves,[])
        finally:await c.disconnect()
    async def test_lock_backlog_does_not_send_a_stale_motion(self):
        c=UsbClient('usb:fake',serial_factory=FakeSerial);await c.connect()
        try:
            c.challenge=(7,1);c.last_seen=c.last_radio=time.monotonic()
            await c.tx_lock.acquire();task=asyncio.create_task(c.write_gatt_char(None,command(2,40,10)))
            await asyncio.sleep(.13);c.tx_lock.release();await task
            self.assertFalse(any(b[3]==2 for _,b in Parser().feed(b''.join(c.serial.writes))))
        finally:await c.disconnect()
    async def test_wave_traffic_never_renews_radio_status_lease(self):
        c=UsbClient('usb:fake',serial_factory=FakeSerial);await c.connect()
        try:
            c.challenge=(7,1);c.control_ready=True;c.last_seen=time.monotonic()-.61
            for n in range(100):c.receive(4,wave_body(c,pulse_packets()[0],0,start=n))
            await asyncio.sleep(.02);self.assertFalse(c.control_ready);self.assertIsNone(c.challenge)
        finally:await c.disconnect()

class LossySerial(RadioSerial):
    instances=[]
    def __init__(self,*a,**kw):
        super().__init__();self.silent=False;self.wave_load=False;self.drop=0;self.epoch=1;self.all_commands=[];type(self).instances.append(self)
    def status(self,query=False):
        if self.silent or self.drop>0:
            if self.drop:self.drop-=1
            return
        before=len(self.rx);super().status(query)
        packet=bytearray(self.rx[before:]);body=packet[4:68]
        struct.pack_into('<I',body,12,self.epoch);struct.pack_into('<I',body,60,zlib.crc32(body[:60]))
        del self.rx[before:];self.rx.extend(usb_frame(2,body))
    def write(self,data):
        for _,b in Parser().feed(data):
            if b[3]==2:self.all_commands.append(bytes(b))
        return super().write(data)
    def read(self,n):
        if self.closed:raise OSError('USB unplug fault injection')
        if self.wave_load and self.session:
            for raw in pulse_packets():
                body=bytearray(wn_frame(4,1,7,self.epoch,self.session));body[40:60]=raw
                struct.pack_into('<I',body,60,zlib.crc32(body[:60]));self.rx.extend(usb_frame(4,body))
        return super().read(n)

class ActiveRecovery(unittest.IsolatedAsyncioTestCase):
    async def start_controller(self):
        c=CombinedController.__new__(CombinedController);c.events=queue.Queue(maxsize=10000)
        c.client_factory=lambda device,**kw:UsbClient(device,serial_factory=LossySerial,**kw)
        c.devices={'usb:fake':'usb:fake'};c.client=c.session=None;c.ui_at=time.monotonic()
        task=asyncio.create_task(c.connect('usb:fake'));return c,task
    async def until(self,c,task,predicate,timeout=4):
        end=time.monotonic()+timeout
        while not predicate():
            c.ui_at=time.monotonic()
            if task.done():await task;self.fail('controller unexpectedly ended')
            if time.monotonic()>end:self.fail('condition not reached')
            await asyncio.sleep(.01)
    async def test_continuous_wave_drop_and_radio_loss_recovery_never_restart(self):
        c,task=await self.start_controller()
        try:
            await self.until(c,task,c.available);s=c.client.serial;s.wave_load=True;s.drop=2
            c._request('move',(1,40));await self.until(c,task,lambda:any(x[25]==1 for x in s.all_commands))
            await asyncio.sleep(.3);self.assertTrue(c.available())
            s.silent=True;await self.until(c,task,lambda:c.held is None and not c.available())
            s.session=0;s.seq=0;s.motion=0;s.speed=0;s.epoch+=1;marker=len(s.all_commands);s.silent=False
            await self.until(c,task,c.available);await asyncio.sleep(.45)
            self.assertIsNone(c.held);self.assertTrue(all(x[25]==0 for x in s.all_commands[marker:]))
            c._request('move',(2,40));await self.until(c,task,lambda:any(x[25]==2 for x in s.all_commands[marker:]))
        finally:task.cancel();await asyncio.gather(task,return_exceptions=True)
    async def test_usb_reopens_with_a_new_session_and_only_stop(self):
        c,task=await self.start_controller()
        try:
            await self.until(c,task,c.available);old=c.client.serial;old_session=c.client.session
            c._request('move',(1,40));await self.until(c,task,lambda:any(x[25]==1 for x in old.all_commands))
            old.closed=True
            await self.until(c,task,lambda:c.client is not None and getattr(c.client,'serial',old) is not old and c.available())
            fresh=c.client.serial;self.assertNotEqual(c.client.session,old_session)
            await asyncio.sleep(.3);self.assertTrue(fresh.all_commands);self.assertTrue(all(x[25]==0 for x in fresh.all_commands))
            self.assertIsNone(c.held)
        finally:task.cancel();await asyncio.gather(task,return_exceptions=True)
