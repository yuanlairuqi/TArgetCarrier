import asyncio
import csv
import io
import queue
import struct
import sys
import time
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from desktop import Bridge
from usb_transport import CombinedController, UsbClient, usb_frame, wn_frame
from test_desktop import Recorder
from test_usb import state, RadioSerial


class LeaseSerial(RadioSerial):
    """Software emulator with the real 800 ms idle lease and epoch invalidation."""
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.epoch = 1
        self.last_command = time.monotonic()
        self.expirations = 0

    def invalidate(self):
        self.epoch += 1; self.session = 0; self.seq = 0
        self.motion = self.speed = 0; self.expirations += 1

    def status(self, query=False):
        b = bytearray(wn_frame(3, 1, 7, self.epoch, 0 if query else self.session, self.seq))
        b[24:27] = bytes([0, 2 if self.session else 0, 1 if query else 0])
        flags = 185 if self.session else 153
        b[40:60] = bytes([3])+struct.pack('<BBHBBBIIHBB', self.motion, flags, 65535,
            self.motion, self.speed, 0, self.seq, self.seq, 1, 0, self.motion)
        struct.pack_into('<I', b, 60, zlib.crc32(b[:60]))
        self.rx.extend(usb_frame(2, b))

    def write(self, data):
        self.last_command = time.monotonic()
        return super().write(data)

    def read(self, n):
        if self.session and time.monotonic()-self.last_command > .8:
            self.invalidate()
        return super().read(n)


class IdleLeaseTests(unittest.IsolatedAsyncioTestCase):
    async def test_idle_over_two_seconds_then_epoch_recovery_never_resumes_motion(self):
        c = CombinedController.__new__(CombinedController)
        c.events = queue.Queue(maxsize=5000)
        c.client_factory = lambda device, **kw: UsbClient(device, serial_factory=LeaseSerial, **kw)
        c.devices = {'usb:fake': 'usb:fake'}
        c.client = c.session = None; c.ui_at = time.monotonic()
        task = asyncio.create_task(c.connect('usb:fake'))
        async def until(condition):
            end = time.monotonic()+2
            while not condition():
                c.ui_at = time.monotonic()
                if task.done():
                    await task
                    self.fail('USB session ended unexpectedly')
                if time.monotonic() > end: self.fail('recovery timed out')
                await asyncio.sleep(.01)
        try:
            await until(c.available)
            serial = c.client.serial
            await asyncio.sleep(2.2)
            await until(c.available)
            self.assertEqual(serial.expirations, 0)
            self.assertGreaterEqual(len(serial.commands), 5)
            seqs = [struct.unpack_from('<I', p, 4)[0] for p in serial.commands]
            self.assertEqual(seqs, sorted(set(seqs)))
            self.assertTrue(all(p[1] == 0 for p in serial.commands))
            c.held = (1, 40); c.next_send = 0
            await until(lambda: any(p[1] == 1 for p in serial.commands))
            serial.invalidate()
            marker = len(serial.commands)
            await until(lambda: c.client.challenge == (7, 2) and c.held is None and c.available())
            await asyncio.sleep(.5)
            self.assertTrue(all(p[1] == 0 for p in serial.commands[marker:]))
            self.assertIsNone(c.held)
        finally:
            task.cancel(); await asyncio.gather(task, return_exceptions=True)


def capture(ident):
    return dict(id=ident, flags=1, initial=0, hz=1000000, duration=20000,
                edges=[[100, 2], [200, 0]], source='slave MCPWM GPIO capture',
                node=1, boot=7, start_ms=12)


class SnapshotTests(unittest.TestCase):
    def setUp(self):
        self.b = Bridge(Recorder())

    def test_freeze_clipboard_and_export_use_same_snapshot(self):
        self.b._events.put(('pulse', capture(7)))
        self.b.refresh(False)
        self.b._events.put(('pulse', capture(8)))
        frozen = self.b.freeze('pulse', True)
        self.b._events.put(('pulse', capture(9)))
        text = self.b.copy_text('pulse')
        self.assertEqual(frozen['capture']['id'], 8)
        self.assertEqual(text, self.b._export_text('pulse'))
        self.assertEqual(next(csv.reader(io.StringIO(text)))[:2], ['# capture', '8'])
        self.assertIn('slave MCPWM GPIO capture', text)
        self.assertIn('received_at', text)

    def test_freeze_is_deep_snapshot(self):
        self.b._events.put(('pulse', capture(7)))
        snap = self.b.freeze('pulse', True)
        snap['capture']['edges'][0][0] = 999
        self.assertEqual(self.b._frozen_capture['edges'][0][0], 100)

    def test_refresh_does_not_expose_mutable_live_capture(self):
        self.b._events.put(('pulse', capture(7)))
        snap = self.b.refresh(False)
        snap['capture']['id'] = 999
        self.assertEqual(self.b._state['capture']['id'], 7)

    def test_disconnect_marks_frozen_export_historical(self):
        self.b._events.put(('pulse', capture(7)))
        self.b.freeze('pulse', True)
        self.b._events.put(('disconnected', None))
        rows = list(csv.reader(io.StringIO(self.b.copy_text('pulse'))))
        self.assertEqual(rows[1][-2:], ['historical', 'True'])

    def test_frozen_raw_clipboard_matches_display(self):
        self.b._log('事件', 'before')
        snap = self.b.freeze('status', True)
        self.b._log('事件', 'after')
        self.assertEqual(len(snap['raw']), 1)
        self.assertIn('before', self.b.copy_text('raw'))
        self.assertNotIn('after', self.b.copy_text('raw'))
        self.b.clear('raw')
        self.assertEqual(self.b.copy_text('raw'), '')

    def test_session_reset_clears_old_usb_transport_and_connection(self):
        self.b._state.update(connected=True, transport={'name': 'USB'}, running=True)
        self.b._events.put(('reset', None))
        s = self.b.refresh(False)
        self.assertFalse(s['connected'])
        self.assertIsNone(s['transport'])
        self.assertGreater(s['session_started_at'], 0)

    def test_usb_status_does_not_claim_ble_connection(self):
        self.b._events.put(('transport', {'name': 'USB', 'diagnostic': 'ok'}))
        self.b._events.put(('status', state()))
        s = self.b.refresh(False)
        self.assertIn('USB → GEEK → ESP-NOW', s['link'])
        self.assertNotIn('BLE 正常', s['link'])


class ScanTests(unittest.IsolatedAsyncioTestCase):
    async def test_usb_only_scan_never_invokes_ble(self):
        c = CombinedController.__new__(CombinedController)
        c.events = queue.Queue()
        c.devices = {'stale': object()}
        c.report = lambda _: None
        from serial.tools import list_ports
        with patch.object(list_ports, 'comports', return_value=[SimpleNamespace(device='COM9', description='USB JTAG serial')]):
            await c.scan(include_ble=False)
        self.assertEqual(c.devices, {'usb:COM9': 'usb:COM9'})
        events = list(c.events.queue)
        rows = next(value for kind, value in events if kind == 'devices')
        self.assertIn('USB JTAG serial', rows[0][1])
        self.assertIsNone(rows[0][2])
        self.assertEqual(events[-1], ('busy', False))

    async def test_usb_scan_exception_releases_busy(self):
        c = CombinedController.__new__(CombinedController)
        c.events = queue.Queue(); c.devices = {}; messages = []
        c.report = messages.append
        from serial.tools import list_ports
        with patch.object(list_ports, 'comports', side_effect=OSError('port enumeration failed')):
            await c.scan(include_ble=False)
        self.assertIn('USB 搜索失败', messages[0])
        self.assertEqual(list(c.events.queue)[-1], ('busy', False))


if __name__ == '__main__':
    unittest.main()
