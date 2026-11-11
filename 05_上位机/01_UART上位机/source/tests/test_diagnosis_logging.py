"""Tests for the bounded diagnosis timeline and the EXT1/EXT2 decoders.

These tests only exercise evidence/logging code: nothing here may change the
control/safety behaviour covered by the existing regression suite.
"""
import asyncio
import logging
import os
import struct
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import controller
import diag_timeline
from controller import Controller
from diag_timeline import MAX_EVENTS, MAX_LINES_PER_SEC, Timeline
from ext_diag import EXT1, EXT2, EXT3, parse_ext1, parse_ext2, parse_ext3, derive_rates
from protocol import Channel, parse_status


class Capture(logging.Handler):
    def __init__(self):
        super().__init__()
        self.lines = []
        self.warnings = []
        self.setLevel(logging.DEBUG)

    def emit(self, record):
        self.lines.append(record.getMessage())
        if record.levelno >= logging.WARNING:
            self.warnings.append(record.getMessage())


def new_logger(name):
    logger = logging.getLogger('tc.test.' + name)
    logger.handlers[:] = []
    logger.propagate = False
    capture = Capture()
    logger.addHandler(capture)
    logger.setLevel(logging.DEBUG)
    return logger, capture


def ext1_frame(**over):
    b = bytearray(20)
    b[0], b[1] = 0xE1, 1
    struct.pack_into('<H', b, 2, over.get('status_attempt', 120))
    struct.pack_into('<H', b, 4, over.get('status_fail', 20))
    struct.pack_into('<H', b, 6, over.get('status_enomem', 5))
    struct.pack_into('<H', b, 8, over.get('status_mbuf_null', 3))
    struct.pack_into('<H', b, 10, over.get('pulse_ok', 4000))
    struct.pack_into('<H', b, 12, over.get('pulse_fail', 40))
    struct.pack_into('<H', b, 14, over.get('pulse_enomem', 7))
    b[16] = over.get('pulse_mbuf_null', 9)
    b[17] = over.get('last_fail_kind', 2)
    b[18] = over.get('last_fail_rc', 6)
    b[19] = over.get('last_fail_age_ds', 12)
    return bytes(b)


def ext2_frame(**over):
    b = bytearray(20)
    b[0], b[1] = 0xE2, 1
    struct.pack_into('<I', b, 2, over.get('ble_cmd_count', 123456))
    struct.pack_into('<I', b, 6, over.get('ble_cmd_last_seq', 0x01020304))
    struct.pack_into('<H', b, 10, over.get('ble_cmd_last_age_ms', 210))
    struct.pack_into('<H', b, 12, over.get('uart_tx_ok', 65432))
    struct.pack_into('<H', b, 14, over.get('uart_tx_fail', 11))
    struct.pack_into('<H', b, 16, over.get('att_lock_wait_max_ms', 37))
    b[18] = over.get('att_lock_wait_last_ms', 21)
    b[19] = over.get('telemetry_gap_max_ms', 44)
    return bytes(b)


def ext3_frame(**over):
    b = bytearray(20)
    b[0], b[1] = 0xE3, 1
    b[2] = over.get('conn_itvl_ms', 15)
    b[3] = over.get('conn_latency', 0)
    struct.pack_into('<H', b, 4, over.get('att_mtu', 247))
    struct.pack_into('<H', b, 6, over.get('msys_free_min', 5))
    struct.pack_into('<H', b, 8, over.get('msys_free_now', 9))
    struct.pack_into('<H', b, 10, over.get('msys_total', 32))
    struct.pack_into('<H', b, 12, over.get('uart_rx_events', 54321))
    struct.pack_into('<H', b, 14, over.get('diag_ok', 600))
    struct.pack_into('<H', b, 16, over.get('diag_fail', 0))
    struct.pack_into('<H', b, 18, over.get('pulse_transfer_abort_count', 3))
    return bytes(b)


def status_frame(seq=8, op=1, speed=80, phase=2, hz=1636, motion=1, mode=0, flags=0xB9):
    b = bytearray(20)
    b[:3] = bytes([4, motion, flags])
    struct.pack_into('<H', b, 3, hz)
    b[5:8] = bytes([op, speed, 0])
    struct.pack_into('<IIHBB', b, 8, seq, seq, 1000 if speed else 0, mode, phase)
    return parse_status(bytes(b))


class ExtDiagTests(unittest.TestCase):
    def test_ext1_decodes_every_contracted_field(self):
        d = parse_ext1(ext1_frame())
        self.assertEqual((d['status_attempt'], d['status_fail'], d['status_enomem']), (120, 20, 5))
        self.assertEqual((d['status_mbuf_null'], d['status_ok']), (3, 100))
        self.assertEqual((d['pulse_ok'], d['pulse_fail'], d['pulse_enomem']), (4000, 40, 7))
        self.assertEqual((d['pulse_attempt'], d['pulse_mbuf_null']), (4040, 9))
        self.assertEqual((d['last_fail_kind'], d['last_fail_rc'], d['last_fail_age_ms']), (2, 6, 1200))

    def test_ext2_decodes_every_contracted_field(self):
        d = parse_ext2(ext2_frame())
        self.assertEqual(d['ble_cmd_count'], 123456)
        self.assertEqual(d['ble_cmd_last_seq'], 0x01020304)
        self.assertEqual(d['ble_cmd_last_age_ms'], 210)
        self.assertEqual((d['uart_tx_ok'], d['uart_tx_fail']), (65432, 11))
        self.assertEqual((d['att_lock_wait_max_ms'], d['att_lock_wait_last_ms']), (37, 21))
        self.assertEqual(d['telemetry_gap_max_ms'], 44)

    def test_ext3_decodes_every_contracted_field(self):
        d = parse_ext3(ext3_frame())
        self.assertEqual((d['conn_itvl_ms'], d['conn_latency']), (15, 0))
        self.assertEqual(d['att_mtu'], 247)
        self.assertEqual((d['msys_free_min'], d['msys_free_now'], d['msys_total']), (5, 9, 32))
        self.assertEqual(d['uart_rx_events'], 54321)
        self.assertEqual((d['diag_ok'], d['diag_fail']), (600, 0))
        self.assertEqual(d['pulse_transfer_abort_count'], 3)

    def test_derive_rates_from_consecutive_samples(self):
        first = parse_ext1(ext1_frame(status_attempt=100, status_fail=5, pulse_ok=1000, pulse_fail=10))
        second = parse_ext1(ext1_frame(status_attempt=110, status_fail=15, pulse_ok=1400, pulse_fail=20))
        rates = derive_rates('ext1', first, second, 1.0)
        self.assertEqual(rates['status_attempt_per_s'], 10.0)
        self.assertEqual(rates['status_fail_per_s'], 10.0)
        self.assertEqual(rates['pulse_ok_per_s'], 400.0)
        self.assertEqual(rates['pulse_fail_per_s'], 10.0)
        self.assertEqual(rates['pulse_attempt_per_s'], 410.0)
        self.assertEqual(rates['status_ok_per_s'], 0.0)
        self.assertEqual(derive_rates('ext1', None, second, 1.0), {})

    def test_unknown_marker_layout_or_length_is_rejected(self):
        for frame in (ext1_frame()[:-1], bytes([0xE2]) + ext1_frame()[1:], ext1_frame()[:1] + bytes([2]) + ext1_frame()[2:]):
            with self.assertRaises(ValueError):
                parse_ext1(frame)
        for frame in (ext2_frame()[:-1], bytes([0xE1]) + ext2_frame()[1:], ext2_frame()[:1] + bytes([9]) + ext2_frame()[2:]):
            with self.assertRaises(ValueError):
                parse_ext2(frame)
        for frame in (ext3_frame()[:-1], bytes([0xE2]) + ext3_frame()[1:], ext3_frame()[:1] + bytes([5]) + ext3_frame()[2:]):
            with self.assertRaises(ValueError):
                parse_ext3(frame)

    def test_uuids_are_the_contracted_characteristics(self):
        self.assertEqual(EXT1, '6e400006-b5a3-f393-e0a9-e50e24dcca9e')
        self.assertEqual(EXT2, '6e400007-b5a3-f393-e0a9-e50e24dcca9e')
        self.assertEqual(EXT3, '6e400008-b5a3-f393-e0a9-e50e24dcca9e')


class TimelineTests(unittest.TestCase):
    def setUp(self):
        self.logger, self.capture = new_logger(self.id().rsplit('.', 1)[-1])
        self.now = [0.0]
        self.timeline = Timeline(logger=self.logger, clock=lambda: self.now[0], enabled=True, ext_enabled=True)

    def lines(self):
        return list(self.capture.lines)

    def test_disabled_timeline_is_a_no_op(self):
        timeline = Timeline(logger=self.logger, clock=lambda: self.now[0], enabled=False, ext_enabled=False)
        timeline.write_submit(1, 1, 1)
        timeline.write_done(1, 250)
        timeline.notify('pulse', 20)
        timeline.quarantine('test')
        self.assertEqual(self.lines(), [])
        self.assertEqual(dict(timeline.counts), {})

    def test_switches_read_environment(self):
        saved = {k: os.environ.get(k) for k in ('TC_DIAG_TIMELINE', 'TC_DIAG_EXT')}
        try:
            os.environ['TC_DIAG_TIMELINE'] = '0'
            os.environ['TC_DIAG_EXT'] = 'off'
            timeline = Timeline(logger=self.logger)
            self.assertFalse(timeline.enabled)
            self.assertFalse(timeline.ext_enabled)
            os.environ['TC_DIAG_TIMELINE'] = '1'
            os.environ['TC_DIAG_EXT'] = '1'
            timeline = Timeline(logger=self.logger)
            self.assertTrue(timeline.enabled)
            self.assertTrue(timeline.ext_enabled)
        finally:
            for key, value in saved.items():
                if value is None:
                    os.environ.pop(key, None)
                else:
                    os.environ[key] = value

    def test_write_lifecycle_lines_and_counters(self):
        self.timeline.write_submit(11, 1, 1)
        self.now[0] = 0.2
        self.timeline.write_done(11, 203, ok=True)
        self.now[0] = 0.25
        self.timeline.write_submit(12, 1, 1)
        self.now[0] = 1.7
        self.timeline.write_deadline(12, 1520, 1500)
        self.timeline.quarantine('write_deadline')
        self.timeline.write_native(12, 'cancelled')
        text = '\n'.join(self.lines())
        self.assertIn('X TX submit seq=11 op=1 stop_seq=1', text)
        self.assertIn('X TX done seq=11 ok=1 ms=203', text)
        self.assertIn('X TX deadline seq=12 elapsed_ms=1520 deadline_ms=1500 quarantined=1', text)
        self.assertIn('QUARANTINE reason=write_deadline old_session_fenced=1', text)
        self.assertIn('X TX native seq=12 state=cancelled', text)
        self.assertEqual(self.timeline.counts['write_submit'], 2)
        self.assertEqual(self.timeline.counts['write_done'], 1)
        self.assertEqual(self.timeline.counts['write_deadline'], 1)
        self.assertEqual(self.timeline.counts['quarantine'], 1)

    def test_write_failure_line_records_exception_type(self):
        self.timeline.write_done(5, 12.4, ok=False, kind='CancelledError')
        self.assertIn('X TX fail seq=5 ok=0 kind=CancelledError elapsed_ms=12', '\n'.join(self.lines()))
        self.assertEqual(self.timeline.counts['write_fail'], 1)

    def test_line_rate_limit_and_bounded_ring(self):
        for index in range(MAX_LINES_PER_SEC * 5):
            self.timeline.write_submit(index, 1, 1)
        self.assertLessEqual(len(self.lines()), MAX_LINES_PER_SEC)
        self.assertGreater(self.timeline.suppressed, 0)
        for index in range(MAX_EVENTS + 200):
            self.timeline.write_submit(index, 1, 1)
        self.assertEqual(len(self.timeline.events), MAX_EVENTS)

    def test_notification_rates_and_pulse_peak(self):
        for _ in range(10):
            self.timeline.notify('status', 20)
        for _ in range(200):
            self.timeline.notify('pulse', 20)
        self.timeline.notify('diag', 20)
        self.now[0] = 1.0
        self.timeline.second(self.now[0], status_frame())
        text = '\n'.join(self.lines())
        self.assertIn('status=10.0/s', text)
        self.assertIn('pulse=200.0/s', text)
        self.assertIn('diag=1.0/s', text)
        self.assertIn('pulse_bytes=4000/s', text)
        self.assertIn('pulse_peak_100ms=2000/s', text)

    def test_ack_stall_warns_once_per_threshold_per_episode(self):
        moving = SimpleNamespace(ack=7, stm_ack=7, motion=1, requested=1)
        stalled = SimpleNamespace(ack=7, stm_ack=7, motion=1, requested=1)
        self.now[0] = 0.0
        self.timeline.status_ack(0.0, moving)
        self.timeline.status_ack(0.31, stalled)
        self.timeline.status_ack(0.55, stalled)
        self.timeline.status_ack(0.60, stalled)          # already warned for both
        self.assertIn('threshold=300', '\n'.join(self.lines()))
        self.assertIn('threshold=500', '\n'.join(self.lines()))
        self.assertEqual(sum(1 for line in self.lines() if line.startswith('ACK stall')), 2)
        advanced = SimpleNamespace(ack=8, stm_ack=8, motion=1, requested=1)
        self.timeline.status_ack(0.71, advanced)          # new ack clock
        self.timeline.status_ack(1.05, advanced)          # 340 ms of the new episode
        self.assertEqual(sum(1 for line in self.lines() if line.startswith('ACK stall')), 3)

    def test_ack_stall_is_not_reported_when_stopped(self):
        stopped = SimpleNamespace(ack=0, stm_ack=0, motion=0, requested=0)
        self.timeline.status_ack(0.0, stopped)
        self.timeline.status_ack(2.0, stopped)
        self.assertEqual([line for line in self.lines() if line.startswith('ACK stall')], [])

    def test_full_status_line_contains_every_parsed_field(self):
        status = status_frame()
        self.now[0] = 1.0
        self.timeline.second(self.now[0], status)
        line = next(line for line in self.lines() if line.startswith('STATUS '))
        for field in ('version=4', 'motion=1', 'requested=1', 'speed=80', 'mode=0', 'phase=2',
                      'flags=185', 'error=0', 'ack=8', 'stm_ack=8', 'current_hz=1636',
                      'target_hz=1000', 'uptime=0', 'battery=1636', 'uart=1', 'online=1'):
            self.assertIn(field, line)

    def test_ext_samples_log_raw_hex_and_degrade_once(self):
        self.timeline.ext_read('ext1', ext1_frame(), None)
        text = '\n'.join(self.lines())
        self.assertIn('EXT1 layout=1', text)
        self.assertIn('status_attempt=120', text)
        self.assertIn(ext1_frame().hex().upper(), text)
        self.timeline.ext_read('ext2', bytes([0xE2, 9]) + bytes(18), None)   # unknown layout
        self.timeline.ext_read('ext2', bytes([0xE2, 9]) + bytes(18), None)
        self.assertEqual(len([w for w in self.capture.warnings if 'layout unknown' in w]), 1)
        self.timeline.ext_read('ext1', None, 'WriteChannelTimeout')
        self.timeline.ext_read('ext1', None, 'WriteChannelTimeout')
        self.assertEqual(len([w for w in self.capture.warnings if 'EXT read fail' in w]), 1)
        self.assertEqual(self.timeline.counts['ext_bad_layout'], 2)
        self.assertEqual(self.timeline.counts['ext_fail'], 2)

    def test_ext_unavailable_is_logged_once(self):
        self.timeline.ext_unavailable()
        self.timeline.ext_unavailable()
        self.assertEqual(len([w for w in self.capture.warnings if 'EXT diagnostics unavailable' in w]), 1)

    def test_ext_line_contains_derived_rates(self):
        self.timeline.ext_read('ext1', ext1_frame(status_attempt=100, status_fail=5, pulse_ok=1000,
                                                  pulse_fail=10), None, now=0.0)
        self.timeline.ext_read('ext1', ext1_frame(status_attempt=110, status_fail=15, pulse_ok=1400,
                                                  pulse_fail=20), None, now=1.0)
        line = [entry for entry in self.lines() if entry.startswith('EXT1')][-1]
        self.assertIn('status_attempt_per_s=10.0', line)
        self.assertIn('status_ok_per_s=0.0', line)
        self.assertIn('pulse_ok_per_s=400.0', line)
        self.assertIn('pulse_attempt_per_s=410.0', line)

    def test_pulse_capture_line_is_bounded(self):
        capture = dict(id=3, flags=1, hz=1000, duration=1000, pul=10, direction=11, edges=(1, 2, 3))
        self.timeline.pulse_capture(0.0, capture)
        self.timeline.pulse_capture(0.2, capture)
        self.assertEqual(len([line for line in self.lines() if line.startswith('PULSE capture')]), 1)


class FakeExtClient:
    def __init__(self, hang=False):
        self.hang = hang
        self.reads = []

    async def read_gatt_char(self, characteristic):
        self.reads.append(characteristic)
        if self.hang:
            await asyncio.sleep(30)
        return {EXT1: ext1_frame(), EXT2: ext2_frame(), EXT3: ext3_frame()}.get(characteristic, b'')


class ExtPollingTests(unittest.IsolatedAsyncioTestCase):
    def owner(self, hang=False):
        logger, capture = new_logger(self.id().rsplit('.', 1)[-1])
        c = Controller.__new__(Controller)
        c.timeline = Timeline(logger=logger, clock=lambda: 0.0, enabled=True, ext_enabled=True)
        c.client = FakeExtClient(hang=hang)
        c.ext = {'ext1': EXT1, 'ext2': EXT2, 'ext3': EXT3}
        c.ext_read_timeout = 0.05
        c._ext_owner = SimpleNamespace()
        c.channel = Channel()
        c.held = None
        c.barrier = False
        c._ext_task = None
        return c, capture

    async def test_poll_reads_both_characteristics(self):
        c, capture = self.owner()
        await c._poll_ext(0.0)
        self.assertEqual(c.client.reads, [EXT1, EXT2, EXT3])
        text = '\n'.join(capture.lines)
        self.assertIn('EXT1 layout=1', text)
        self.assertIn('EXT2 layout=1', text)
        self.assertIn('EXT3 layout=1', text)

    async def test_poll_degrades_when_read_hangs(self):
        c, capture = self.owner(hang=True)
        await asyncio.wait_for(c._poll_ext(0.0), timeout=2.0)
        self.assertGreaterEqual(c.timeline.counts['ext_fail'], 1)
        self.assertTrue(any('EXT read fail' in line for line in capture.warnings))

    async def test_ext_tick_never_starts_while_moving_or_blocked(self):
        c, _ = self.owner()
        c.held = (1, 40, 0, 0, 0, 200, 100)
        c._ext_tick(1000.0)
        self.assertIsNone(c._ext_task)
        c.held = None
        c.channel.barrier = True
        c._ext_tick(1000.0)
        self.assertIsNone(c._ext_task)
        c.channel.barrier = False
        c.channel.pending = object()
        c._ext_tick(1000.0)
        self.assertIsNone(c._ext_task)
        c.channel.pending = None
        c._ext_tick(1000.0)
        task = c._ext_task
        self.assertIsNotNone(task)
        c._ext_tick(1000.5)          # still in flight: no second read
        self.assertIs(c._ext_task, task)
        task.cancel()
        await asyncio.gather(task, return_exceptions=True)


if __name__ == '__main__':
    unittest.main()
