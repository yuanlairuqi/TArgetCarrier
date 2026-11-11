"""Limit is a valid state, not a transport failure. No real BLE/serial I/O."""
import asyncio
import queue
import struct
import time
import unittest
import sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from controller import Controller
from protocol import Channel, parse_status
from usb_transport import CombinedController, UsbClient
from test_ramp import status, Services


def limited(seq, error=10, op=0, speed=0, phase=0, hz=0, motion=0):
    data = bytearray(status(seq, op, speed, phase, hz, motion))
    data[7] = error
    return bytes(data)


class CodecTests(unittest.TestCase):
    def test_limit_error_is_valid_v3_and_v4_but_never_move_ready(self):
        for version in (3, 4):
            b = bytearray(limited(1)); b[0] = version
            s = parse_status(b)
            self.assertFalse(s.can_move)
            self.assertTrue(s.stop_feedback)
        b = bytearray(limited(1)); b[2] = 3  # even demo may not ignore a limit
        self.assertFalse(parse_status(b).can_move)

    def test_unknown_error_remains_invalid(self):
        with self.assertRaises(ValueError): parse_status(limited(1, error=11))

    def test_limit_stop_ack_clears_sync_barrier_but_not_motion_lock(self):
        c = Channel(ramp=True); c.stop(0); p = c.take(0); c.writing = None
        c.status(parse_status(limited(p.seq)))
        self.assertTrue(c.synced); self.assertFalse(c.barrier)
        self.assertFalse(parse_status(limited(p.seq)).can_move)

    def test_limit_stop_does_not_accept_moving_or_wrong_ack_feedback(self):
        c = Channel(ramp=True); c.stop(0); p = c.take(0); c.writing = None
        c.status(parse_status(limited(p.seq, phase=3, hz=100, motion=1)))
        self.assertTrue(c.barrier)
        c.status(parse_status(limited(p.seq+1)))
        self.assertTrue(c.barrier)

    def test_limit_error_cannot_confirm_movement(self):
        c = Channel(ramp=True); c.synced = True; c.move(1, 40, 0)
        p = c.take(0); c.writing = None
        c.status(parse_status(limited(p.seq, op=1, speed=40, phase=1, hz=100, motion=1)))
        self.assertTrue(c.unconfirmed)

    def test_reset_is_wire_compatible_stop_extension(self):
        c = Channel(ramp=True); c.synced = True; seq = c.reset_limit(0)
        p = c.take(0)
        self.assertEqual(p.motion, 0)
        self.assertEqual(struct.unpack('<BBBBIHHHH', p.data), (4, 0, 0, 0, seq, 0, 0, 0x4c52, 0xa55a))

    def test_usb_keeps_safe_stop_lease_alive_while_limit_locked(self):
        c = CombinedController.__new__(CombinedController)
        c.client = UsbClient('usb:fake'); c.client.is_connected = True
        c.client.control_ready = True; c.client.last_seen = time.monotonic()
        c.channel = Channel(ramp=True); c.channel.synced = True
        c.status = parse_status(limited(1)); c.held = None; c.closing = False; c.last_stop = 0
        c.idle_maintenance(time.monotonic())
        self.assertTrue(c.channel.barrier)
        self.assertEqual(c.channel.pending.motion, 0)


class LimitClient:
    def __init__(self, *a, **kw):
        self.services = Services(); self.is_connected = False
        self.seq = self.op = self.speed = 0
        self.error = 10; self.released = False; self.writes = []; self.disconnects = 0
    async def connect(self): self.is_connected = True
    async def disconnect(self):
        self.disconnects += 1; self.is_connected = False
        self.task.cancel(); await asyncio.gather(self.task, return_exceptions=True)
    async def start_notify(self, uuid, callback):
        if '0003-' not in uuid: return
        async def loop():
            while self.is_connected:
                callback(None, limited(self.seq, error=self.error, op=self.op, speed=self.speed,
                    phase=self.op if self.op else 0, hz=100 if self.op else 0, motion=self.op))
                await asyncio.sleep(.02)
        self.task = asyncio.create_task(loop())
    async def write_gatt_char(self, tx, data, response=True):
        self.writes.append(bytes(data))
        op = data[1]
        if op and self.error == 10: return  # business reject, no GATT exception
        self.seq = struct.unpack_from('<I', data, 4)[0]
        self.op, self.speed = op, data[2]
        if data[12:16] == struct.pack('<HH', 0x4c52, 0xa55a) and self.released:
            self.error = 0


class ConnectionTests(unittest.IsolatedAsyncioTestCase):
    async def setup_owner(self):
        c = Controller.__new__(Controller); c.events = queue.Queue(5000)
        c.client_factory = LimitClient; c.devices = {'fake': None}
        c.client = c.session = None; c.ui_at = time.monotonic(); c.ramp = True
        self.c = c
        self.session = asyncio.create_task(c.connect('fake'))
        await self.until(lambda: getattr(c, 'channel', None) and c.channel.synced and c.status and c.status.error == 10)
    async def until(self, condition, seconds=2):
        end = time.monotonic() + seconds
        while not condition():
            self.c.ui_at = time.monotonic()
            if self.session.done():
                await self.session
                self.fail('unexpected BLE disconnect')
            if time.monotonic() >= end: self.fail('condition timed out')
            await asyncio.sleep(.02)
    async def asyncTearDown(self):
        if hasattr(self, 'session'):
            self.session.cancel(); await asyncio.gather(self.session, return_exceptions=True)

    async def test_stays_connected_at_limit_and_rejects_motion(self):
        await self.setup_owner(); c = self.c
        await asyncio.sleep(.3)
        self.assertTrue(c.client.is_connected); self.assertEqual(c.client.disconnects, 0)
        self.assertFalse(c.available()); self.assertTrue(c.limit_connected())
        c._request('move', (1, 40, 0, 0, 0, 200, 200))
        self.assertIsNone(c.held)

    async def test_reset_denied_keeps_same_connection_and_stop_confirmation(self):
        await self.setup_owner(); c = self.c; original = c.client
        c._request('limit_reset', ())
        await self.until(lambda: c.limit_reset_seq is None and not c.channel.barrier)
        self.assertIs(c.client, original); self.assertTrue(c.limit_connected())
        self.assertFalse(c.available()); self.assertEqual(c.client.disconnects, 0)

    async def test_released_reset_succeeds_without_reconnect_or_automatic_motion(self):
        await self.setup_owner(); c = self.c; original = c.client
        c.client.released = True; c._request('limit_reset', ())
        await self.until(lambda: c.available() and c.limit_reset_seq is None)
        await asyncio.sleep(.15)
        self.assertIs(c.client, original); self.assertEqual(c.client.disconnects, 0)
        self.assertIsNone(c.held)
        self.assertTrue(all(data[1] == 0 for data in c.client.writes))

    async def test_reset_does_not_send_when_window_lease_expired(self):
        await self.setup_owner(); c = self.c; count = len(c.client.writes)
        c.ui_at = time.monotonic()-1
        c._request('limit_reset', ())
        await asyncio.sleep(.1)
        self.assertEqual(len(c.client.writes), count)


if __name__ == '__main__': unittest.main()
