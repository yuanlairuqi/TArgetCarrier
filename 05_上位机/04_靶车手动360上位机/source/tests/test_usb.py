import asyncio
import struct
import sys
import time
import unittest
import zlib
import queue
from dataclasses import replace
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from usb_transport import Parser,usb_frame,wn_frame,UsbClient
from protocol import Channel,Status,STATUS,PULSE,command,PulseAssembler
from test_control import pulse_packets
from desktop import Bridge
from controller import Controller
from test_desktop import Recorder

def state(seq=1,motion=0,speed=0,flags=185):
    return Status(motion,flags,65535,motion,speed,0,seq,seq,10,0,motion)
def status_body(client,boot=7,epoch=1,owner=2,result=0,seq=1):
    b=bytearray(wn_frame(3,1,boot,epoch,client.session,seq))
    b[24:27]=bytes([result,owner,2]);b[40:60]=bytes([3])+struct.pack('<BBHBBBIIHBB',0,185,65535,0,0,0,seq,seq,10,0,0)
    struct.pack_into('<I',b,60,zlib.crc32(b[:60]));return b
def wave_body(client,raw,index,total=3,start=10,boot=7,session=None):
    b=bytearray(wn_frame(4,1,boot,1,client.session if session is None else session))
    struct.pack_into('<IHH',b,24,start,index,total);b[32]=1;b[40:60]=raw
    struct.pack_into('<I',b,60,zlib.crc32(b[:60]));return b

class USBCodecTests(unittest.TestCase):
    def test_split_coalesced_and_resync(self):
        p=Parser();b=wn_frame(1,1);frame=usb_frame(1,b)
        self.assertEqual(p.feed(b'bad'+frame[:8]),[])
        self.assertEqual(p.feed(frame[8:]+frame),[(1,b),(1,b)])
        self.assertLess(len(p.buffer),72)
    def test_bad_crc_version_and_bounded_noise(self):
        p=Parser();b=bytearray(usb_frame(1,wn_frame(1,1)));b[-1]^=1
        self.assertEqual(p.feed(bytes(b)*10+b'x'*10000),[])
        self.assertLess(len(p.buffer),72)
        self.assertEqual(len(p.feed(usb_frame(1,wn_frame(1,1)))),1)
    def test_wrong_node_version_crc_are_ignored(self):
        c=UsbClient('usb:fake');seen=[];c.callbacks[STATUS]=lambda _,b:seen.append(b)
        b=status_body(c);b[4]=2;struct.pack_into('<I',b,60,zlib.crc32(b[:60]));c.receive(2,b)
        b=status_body(c);b[2]=2;c.receive(2,b)
        b=status_body(c);b[41]^=1;c.receive(2,b)
        self.assertEqual(seen,[])
    def test_challenge_and_wrong_session_cannot_ack(self):
        c=UsbClient('usb:fake');seen=[];c.callbacks[STATUS]=lambda _,b:seen.append(bytes(b))
        before=c.session;c.receive(2,status_body(c));self.assertNotEqual(c.session,before)
        self.assertFalse(seen[-1][2]&32);self.assertEqual(seen[-1][8:16],bytes(8))
        c.receive(2,status_body(c));self.assertTrue(seen[-1][2]&32)
        b=status_body(c);struct.pack_into('<I',b,16,c.session^123);struct.pack_into('<I',b,60,zlib.crc32(b[:60]));c.receive(2,b)
        self.assertFalse(seen[-1][2]&32)
    def test_busy_owner_cannot_unlock(self):
        c=UsbClient('usb:fake');c.challenge=(7,1);seen=[];c.callbacks[STATUS]=lambda _,b:seen.append(b)
        c.receive(2,status_body(c,owner=1));self.assertEqual(seen[-1][7],3)
        self.assertFalse(seen[-1][2]&32)
    def test_complete_wave_and_source_time(self):
        c=UsbClient('usb:fake');c.challenge=(7,1);a=PulseAssembler();c.callbacks[PULSE]=lambda _,b:a.receive(b)
        for i,p in enumerate(pulse_packets()):c.receive(4,wave_body(c,p,i))
        self.assertEqual(a.latest['id'],7);self.assertEqual(c.capture_meta['start_ms'],10)
        self.assertEqual(c.capture_meta['source'],'slave MCPWM GPIO capture')
    def test_wave_loss_duplicate_wrong_session_and_recovery(self):
        for indexes in [(0,2),(0,1,1,2)]:
            c=UsbClient('usb:fake');c.challenge=(7,1);a=PulseAssembler();c.callbacks[PULSE]=lambda _,b:a.receive(b)
            packets=pulse_packets()
            for i in indexes:c.receive(4,wave_body(c,packets[i],i))
            self.assertIsNone(a.latest)
            for i,p in enumerate(packets):c.receive(4,wave_body(c,p,i,start=20))
            self.assertIsNotNone(a.latest)
        c=UsbClient('usb:fake');c.challenge=(7,1);a=PulseAssembler();c.callbacks[PULSE]=lambda _,b:a.receive(b)
        c.receive(4,wave_body(c,pulse_packets()[0],0,session=c.session^1));self.assertIsNone(a.header)
    def test_wave_timeout_rejects_tail(self):
        c=UsbClient('usb:fake');c.challenge=(7,1);a=PulseAssembler();c.callbacks[PULSE]=lambda _,b:a.receive(b)
        c.receive(4,wave_body(c,pulse_packets()[0],0));c.wave[2]-=6
        c.receive(4,wave_body(c,pulse_packets()[1],1));self.assertIsNone(a.header)

class ConfirmationTests(unittest.TestCase):
    def test_stop_negative_matrix(self):
        for changes in [dict(flags=153),dict(error=7),dict(mode=1),dict(phase=1),dict(stm_ack=0),dict(requested=1),dict(motion=1)]:
            c=Channel();c.stop(0);c.take(0);c.status(replace(state(),**changes));self.assertTrue(c.barrier)
        c=Channel();c.stop(0);c.take(0);c.status(state());self.assertFalse(c.barrier)
    def test_motion_negative_matrix_keeps_confirmation_timer(self):
        for changes in [dict(flags=153),dict(error=7),dict(mode=1),dict(phase=2),dict(stm_ack=0),dict(requested=2),dict(motion=2),dict(speed=41)]:
            c=Channel();c.synced=True;c.move(1,40,0);c.take(0)
            c.status(replace(state(motion=1,speed=40),**changes));self.assertEqual(len(c.unconfirmed),1)
            self.assertEqual(c.expired(1),1)
    def test_atomic_freeze_uses_same_capture_for_display_and_export(self):
        b=Bridge(Recorder());b._state['capture']=dict(id=7,flags=1,initial=0,hz=1000,duration=100,edges=[])
        b._events.put(('pulse',dict(id=8,flags=1,initial=0,hz=1000,duration=100,edges=[])))
        snap=b.freeze('pulse',True);self.assertEqual(snap['capture']['id'],8)
        b._events.put(('pulse',dict(id=9,flags=1,initial=0,hz=1000,duration=100,edges=[])))
        self.assertIn('# capture,8,',b._export_text('pulse'));self.assertEqual(snap['capture']['id'],8)
    def test_disconnect_marks_history(self):
        b=Bridge(Recorder());b._events.put(('pulse',dict(id=7)));b._events.put(('disconnected',None));s=b.refresh(False)
        self.assertTrue(s['capture']['historical']);self.assertIn('历史',s['pulse_state'])

class FakeSerial:
    def __init__(self,*a,**kw):self.writes=[];self.closed=False
    def write(self,data):self.writes.append(bytes(data));return len(data)
    def read(self,n):return b''
    def close(self):self.closed=True

class USBAsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_forward_no_motion_retry_and_disconnect(self):
        c=UsbClient('usb:fake',serial_factory=FakeSerial);await c.connect()
        try:
            c.challenge=(7,1);c.last_radio=time.monotonic()
            data=command(1,40,5);await c.write_gatt_char(None,data)
            await asyncio.sleep(.03)
            commands=[body for kind,body in Parser().feed(b''.join(c.serial.writes)) if body[3]==2]
            self.assertEqual(len(commands),1);self.assertEqual(commands[0][24:40],data)
        finally:await c.disconnect()

class RadioSerial(FakeSerial):
    """Software-only slave/STM status emulator, no USB/radio/GPIO is opened."""
    def __init__(self,*a,**kw):
        super().__init__();self.parser=Parser();self.rx=bytearray();self.session=0;self.seq=0;self.motion=0;self.speed=0;self.last=0;self.commands=[]
    def status(self,query=False):
        b=bytearray(wn_frame(3,1,7,1,0 if query else self.session,self.seq))
        b[24:27]=bytes([0,2 if self.session else 0,1 if query else 0]);flags=185 if self.session else 153
        b[40:60]=bytes([3])+struct.pack('<BBHBBBIIHBB',self.motion,flags,65535,self.motion,self.speed,0,self.seq,self.seq,1,0,self.motion)
        struct.pack_into('<I',b,60,zlib.crc32(b[:60]));self.rx.extend(usb_frame(2,b))
    def write(self,data):
        self.writes.append(bytes(data))
        for kind,b in self.parser.feed(data):
            if b[3]==1:self.status(query=True)
            elif b[3]==2:
                self.session=struct.unpack_from('<I',b,16)[0];self.seq=struct.unpack_from('<I',b,20)[0]
                self.motion=b[25];self.speed=b[26];self.commands.append(bytes(b[24:40]));self.status()
        return len(data)
    def read(self,n):
        if time.monotonic()-self.last>.05:self.last=time.monotonic();self.status()
        out=bytes(self.rx[:n]);del self.rx[:n];return out

class FullUSBControllerTests(unittest.IsolatedAsyncioTestCase):
    async def test_usb_handshake_move_release_with_existing_controller(self):
        c=Controller.__new__(Controller);c.events=queue.Queue(maxsize=5000)
        c.client_factory=lambda device,**kw:UsbClient(device,serial_factory=RadioSerial,**kw)
        c.devices={'usb:fake':'usb:fake'};c.client=c.session=None;c.ui_at=time.monotonic()
        task=asyncio.create_task(c.connect('usb:fake'))
        async def until(condition):
            end=time.monotonic()+2
            while not condition():
                c.ui_at=time.monotonic()
                if task.done():await task;self.fail('USB controller ended unexpectedly')
                if time.monotonic()>end:self.fail('USB controller did not reach expected state')
                await asyncio.sleep(.01)
        try:
            await until(c.available)
            serial=c.client.serial
            self.assertTrue(serial.commands);self.assertTrue(all(p[1]==0 for p in serial.commands))
            c.held=(1,40);c.next_send=0
            await until(lambda:any(p[1]==1 for p in serial.commands))
            c.stop()
            await until(lambda:not c.channel.barrier)
            index=len(serial.commands);await asyncio.sleep(.25)
            self.assertIsNone(c.held);self.assertTrue(all(p[1]==0 for p in serial.commands[index:]))
        finally:
            task.cancel();await asyncio.gather(task,return_exceptions=True)
        self.assertTrue(serial.closed)
    async def test_no_challenge_queries_instead_of_motion(self):
        c=UsbClient('usb:fake',serial_factory=FakeSerial);await c.connect()
        try:
            await c.write_gatt_char(None,command(1,40,9))
            self.assertTrue(all(b[3]==1 for _,b in Parser().feed(b''.join(c.serial.writes))))
        finally:await c.disconnect()
