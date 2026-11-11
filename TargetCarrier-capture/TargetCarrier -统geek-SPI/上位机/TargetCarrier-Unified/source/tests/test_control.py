import asyncio
import queue
import struct
import sys
import time
import unittest
import zlib
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol import *
from controller import Controller

def status(seq=0, motion=0, flags=1|2, stm=None, requested=None):
    return bytes([3]) + struct.pack('<BBHBBBIIHBB', motion, flags, 65535,
        motion if requested is None else requested, 40 if motion else 0, 0, seq,
        seq if stm is None else stm, 3, 0, motion if motion < 3 else 0)

class WireTests(unittest.TestCase):
    def test_timed_command_and_canonical_stop(self):
        self.assertEqual(command(1, 40, 0x12345678, 1, 1000, 2000).hex(), '0301280178563412e8030000d0070000')
        self.assertEqual(command(0, 0, 9, 1, 100, 100), bytes.fromhex('03000000090000000000000000000000'))
    def test_bad_durations_and_speeds(self):
        for args in [(1,0,1), (0,10,1), (2,101,1), (1,40,1,1,99,100), (1,40,1,1,100,3600001)]:
            with self.assertRaises(ValueError): command(*args)
    def test_status_strict_validation(self):
        self.assertTrue(parse_status(status()).can_move)
        for data in [b'', status()[:-1], bytes([2])+status()[1:], status(flags=1|32), status(flags=1|2|8|32)]:
            with self.assertRaises(ValueError): parse_status(data)
    def test_dual_stop_barrier(self):
        c = Channel(); c.stop(0); p=c.take(0); c.writing=None
        c.status(parse_status(status(p.seq, flags=1|8|16|32|128, stm=0)))
        self.assertFalse(c.synced)
        c.status(parse_status(status(p.seq, flags=1|8|16|32|128)))
        self.assertTrue(c.synced)
        self.assertTrue(c.move(1,40,.1)); self.assertIsNotNone(c.take(.1))
    def test_invented_ack_does_not_unlock(self):
        c=Channel();c.stop(0);c.take(0)
        c.status(parse_status(status(99)))
        self.assertFalse(c.synced)
    def test_stop_preempts_pending_and_stale_motion(self):
        c=Channel();c.synced=True;c.move(1,40,0);c.stop(.1)
        self.assertEqual(c.take(.1).motion,0)
        c=Channel();c.synced=True;c.move(1,40,0)
        self.assertIsNone(c.take(.301))
    def test_ack_expiry_and_sequence_wrap(self):
        c=Channel();c.seq=0xffffffff;c.stop(0);self.assertEqual(c.take(0).seq,0)
        self.assertIsNone(c.expired(.99));self.assertEqual(c.expired(1),0)
        self.assertFalse(c.synced)
    def test_outstanding_motion_limit(self):
        c=Channel();c.synced=True
        for n in range(4):
            c.move(1,40,n*.1);self.assertIsNotNone(c.take(n*.1));c.writing=None
        c.move(1,40,.4);self.assertIsNone(c.take(.4))

def pulse_packets(edges=((10,2),(20,0)), flags=1):
    header=struct.pack('<BBHBBIIHBBBB',0x57,1,7,flags,0,1000,100, len(edges),6,7,1,0)
    data=[];crc=zlib.crc32(header)
    for i in range(0,len(edges),3):
        payload=b''.join(struct.pack('<IB',*e) for e in edges[i:i+3]);crc=zlib.crc32(payload,crc)
        data.append((struct.pack('<BBHB',0x57,2,7,i//3)+payload).ljust(20,b'\0'))
    tail=struct.pack('<BBHHHI',0x57,3,7,len(data),len(edges),crc).ljust(20,b'\0')
    return [header,*data,tail]

class PulseTests(unittest.TestCase):
    def test_valid_capture(self):
        a=PulseAssembler()
        for b in pulse_packets(): a.receive(b)
        self.assertEqual(a.latest['edges'],((10,2),(20,0)))
    def test_missing_packet_and_crc(self):
        for packets in [pulse_packets()[::2], pulse_packets()[:-1]+[pulse_packets()[-1][:8]+b'\0'*12]]:
            a=PulseAssembler()
            for b in packets:a.receive(b)
            self.assertIsNone(a.latest);self.assertGreater(a.errors,0)
    def test_nontransition_and_out_of_window(self):
        for edges in [((10,0),),((100,2),),((20,2),(10,0))]:
            a=PulseAssembler()
            for b in pulse_packets(edges):a.receive(b)
            self.assertIsNone(a.latest)
    def test_overflow_kept_but_not_drawable(self):
        a=PulseAssembler()
        for b in pulse_packets(flags=3):a.receive(b)
        self.assertEqual(a.latest['flags'],3)

class FakeCharacteristic:
    properties=['write']
class FakeService:
    def get_characteristic(self, uuid): return None if uuid == PULSE else FakeCharacteristic()
class FakeServices:
    def get_service(self, uuid): return FakeService()
class FakeClient:
    def __init__(self, device, **kwargs):
        self.is_connected=False;self.services=FakeServices();self.writes=[];self.seq=0;self.motion=0
        self.delay=0;self.ack=True;self.notify=True;self.callback=None;self.updater=None
    async def connect(self):self.is_connected=True
    async def disconnect(self):
        self.is_connected=False
        if self.updater:self.updater.cancel()
    async def start_notify(self, uuid, callback):
        self.callback=callback
        async def update():
            while self.is_connected:
                if self.notify:callback(None,status(self.seq,self.motion))
                await asyncio.sleep(.05)
        self.updater=asyncio.create_task(update())
    async def write_gatt_char(self, tx, data, response):
        self.writes.append(bytes(data));await asyncio.sleep(self.delay)
        if self.ack:self.seq=struct.unpack_from('<I',data,4)[0];self.motion=data[1]

class SessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        # Construct without starting an extra OS thread; run on this test event loop.
        self.events=queue.Queue(maxsize=2000)
        self.c=Controller.__new__(Controller)
        self.c.events=self.events;self.c.client_factory=FakeClient;self.c.devices={'fake':object()}
        self.c.client=self.c.session=None;self.c.ui_at=time.monotonic()
        self.task=asyncio.create_task(self.c.connect('fake'))
        await asyncio.sleep(.15)
    async def asyncTearDown(self):
        self.task.cancel();await asyncio.gather(self.task,return_exceptions=True)
    async def test_initial_stop_before_movement(self):
        self.assertTrue(self.c.channel.synced)
        self.assertEqual(self.c.client.writes[0][1],0)
    async def test_timed_mode_heartbeat_and_stop(self):
        self.c.held=(2,30,1,1000,2000)
        await asyncio.sleep(.3)
        commands=[struct.unpack('<BBBBIII',p) for p in self.c.client.writes if p[1]]
        self.assertTrue(commands);self.assertEqual(commands[-1][1:4],(2,30,1))
        self.assertEqual(commands[-1][-2:],(1000,2000))
        self.c.stop();await asyncio.sleep(.15)
        self.assertEqual(self.c.client.writes[-1][1],0)
    async def test_ui_freeze_stops_heartbeat(self):
        self.c.held=(1,40,0,0,0);self.c.ui_at=time.monotonic()-1
        await asyncio.sleep(.12)
        self.assertIsNone(self.c.held);self.assertEqual(self.c.client.writes[-1][1],0)
    async def test_bad_status_stops_and_disconnects(self):
        client=self.c.client
        self.c.receive_status(None,b'bad')
        await asyncio.wait_for(self.task,1)
        self.assertFalse(client.is_connected)
    async def test_status_stale_disconnects(self):
        client=self.c.client;client.notify=False
        self.c.last_status=time.monotonic()-2
        await asyncio.wait_for(self.task,1)
        self.assertFalse(client.is_connected)
    async def test_missing_movement_ack_stops(self):
        client=self.c.client;client.ack=False
        self.c.held=(1,40,0,0,0)
        # UI remains responsive while application ACKs are missing.
        for _ in range(15):self.c.ui_at=time.monotonic();await asyncio.sleep(.1)
        self.assertIsNone(self.c.held)
        self.assertEqual(client.writes[-1][1],0)
        self.assertTrue(self.c.channel.barrier)

if __name__=='__main__': unittest.main(verbosity=2)
