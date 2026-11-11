"""Regression tests for STOP synchronization after a temporary UART outage."""
import struct
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from protocol import Channel, parse_status


def uart_status(esp_ack, stm_ack, *, online=True, motion=0, requested=0):
    flags = 1 | 128 | ((8 | 16 | 32) if online else 0)
    return parse_status(bytes([3]) + struct.pack(
        '<BBHBBBIIHBB', motion, flags, 65535, requested,
        40 if requested else 0, 0, esp_ack, stm_ack, 1, 0, motion))


class StopRecoveryTests(unittest.TestCase):
    def test_timed_out_stop_retry_has_a_new_confirmation_deadline(self):
        channel = Channel()
        channel.stop(0)
        first = channel.take(0)
        channel.writing = None
        self.assertEqual(channel.expired(1), 0)

        channel.stop(2)
        retry = channel.take(2)
        channel.writing = None
        self.assertNotEqual(retry.seq, first.seq)
        self.assertEqual([p.seq for p in channel.unconfirmed], [retry.seq])
        self.assertIsNone(channel.expired(2.001))
        self.assertIsNone(channel.expired(2.999))
        self.assertEqual(channel.expired(3), 0)

    def test_force_stop_clears_old_confirmations_but_preserves_inflight_write(self):
        channel = Channel()
        channel.synced = True
        channel.move(1, 40, 0)
        old_motion = channel.take(0)

        channel.stop(.1, force=True)
        self.assertIs(channel.writing, old_motion)
        self.assertEqual(list(channel.unconfirmed), [])
        self.assertIsNone(channel.take(.1))
        self.assertTrue(channel.barrier)
        self.assertFalse(channel.timed_out)

        channel.writing = None
        new_stop = channel.take(.2)
        self.assertEqual(new_stop.motion, 0)
        self.assertEqual([p.seq for p in channel.unconfirmed], [new_stop.seq])

    def test_late_old_stop_ack_cannot_unlock_new_stop(self):
        channel = Channel()
        channel.stop(0)
        old_stop = channel.take(0)
        channel.writing = None
        channel.stop(.2, force=True)
        new_stop = channel.take(.2)
        channel.writing = None

        channel.status(uart_status(old_stop.seq, old_stop.seq))
        self.assertTrue(channel.barrier)
        self.assertFalse(channel.synced)
        self.assertEqual([p.seq for p in channel.unconfirmed], [new_stop.seq])

        channel.status(uart_status(new_stop.seq, new_stop.seq))
        self.assertFalse(channel.barrier)
        self.assertTrue(channel.synced)

    def test_uart_recovery_requires_both_esp_and_stm_stop_ack(self):
        channel = Channel()
        channel.stop(0)
        stop = channel.take(0)
        channel.writing = None

        for state in (
            uart_status(stop.seq, 0),
            uart_status(0, stop.seq),
            uart_status(stop.seq, stop.seq, online=False),
            uart_status(stop.seq, stop.seq, motion=1),
            uart_status(stop.seq, stop.seq, requested=1),
        ):
            channel.status(state)
            self.assertTrue(channel.barrier)
            self.assertFalse(channel.synced)

        channel.status(uart_status(stop.seq, stop.seq))
        self.assertFalse(channel.barrier)
        self.assertTrue(channel.synced)

    def test_force_stop_advances_sequence_including_uint32_wrap(self):
        channel = Channel()
        channel.seq = 0xfffffffe
        channel.stop(0)
        self.assertEqual(channel.stop_seq, 0xffffffff)
        channel.stop(.1, force=True)
        self.assertEqual(channel.stop_seq, 0)
        channel.stop(.2, force=True)
        self.assertEqual(channel.stop_seq, 1)


if __name__ == '__main__':
    unittest.main(verbosity=2)
