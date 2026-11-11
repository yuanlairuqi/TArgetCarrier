"""BLE stays up while the downstream STM32 UART disappears and recovers."""
import asyncio
import queue
import struct
import sys
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from controller import Controller
from protocol import COMMAND, PULSE, SERVICE, STATUS, link_description, parse_status


def status_bytes(esp_ack=0, stm_ack=0, *, online=True, uart=True,
                 ready=True, fault=False, motion=0, requested=0, error=0):
    flags = 1
    if uart:
        flags |= 128
        if online:
            flags |= 8 | 32 | (16 if ready else 0) | (64 if fault else 0)
    else:
        flags |= 2
    return bytes([3]) + struct.pack(
        '<BBHBBBIIHBB', motion, flags, 65535, requested,
        40 if requested else 0, error, esp_ack, stm_ack, 1, 0,
        motion if motion in (0, 1, 2) else 0)


class FakeCharacteristic:
    properties = ['write']


class FakeService:
    def get_characteristic(self, uuid):
        return FakeCharacteristic() if uuid in (COMMAND, STATUS) else None


class FakeServices:
    def get_service(self, uuid):
        return FakeService() if uuid == SERVICE else None


class FakeUARTClient:
    def __init__(self, device, **kwargs):
        self.services = FakeServices()
        self.is_connected = False
        self.online = True
        self.auto_stm_ack = True
        self.esp_ack = self.stm_ack = 0
        self.motion = self.requested = 0
        self.writes = []
        self.callback = self.updater = None

    async def connect(self):
        self.is_connected = True

    async def disconnect(self):
        self.is_connected = False
        if self.updater:
            self.updater.cancel()
            await asyncio.gather(self.updater, return_exceptions=True)

    async def start_notify(self, uuid, callback):
        if uuid != STATUS:
            raise AssertionError('This fixture exposes status notifications only')
        self.callback = callback

        async def update():
            while self.is_connected:
                callback(None, status_bytes(
                    self.esp_ack, self.stm_ack, online=self.online,
                    motion=self.motion, requested=self.requested))
                await asyncio.sleep(.03)

        self.updater = asyncio.create_task(update())

    async def write_gatt_char(self, characteristic, data, response):
        self.writes.append(bytes(data))
        self.esp_ack = struct.unpack_from('<I', data, 4)[0]
        self.requested = data[1]
        if self.online and self.auto_stm_ack:
            self.acknowledge_stm_latest()
        await asyncio.sleep(.005)

    def acknowledge_stm_latest(self):
        self.stm_ack = self.esp_ack
        self.motion = self.requested


class UARTRecoverySessionTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.events = queue.Queue(maxsize=5000)
        self.controller = Controller.__new__(Controller)
        self.controller.events = self.events
        self.controller.client_factory = FakeUARTClient
        self.controller.devices = {'fake': object()}
        self.controller.client = self.controller.session = None
        self.controller.ui_at = time.monotonic()
        self.session = asyncio.create_task(self.controller.connect('fake'))

        async def ui_heartbeat():
            while True:
                self.controller.ui_at = time.monotonic()
                await asyncio.sleep(.05)

        self.heartbeat = asyncio.create_task(ui_heartbeat())
        await self.until(self.controller.available)
        self.client = self.controller.client

    async def asyncTearDown(self):
        self.heartbeat.cancel()
        self.session.cancel()
        await asyncio.gather(self.heartbeat, self.session, return_exceptions=True)

    async def until(self, condition, timeout=1):
        deadline = time.monotonic() + timeout
        while not condition():
            if self.session.done():
                await self.session
                self.fail('BLE session ended before the expected transition')
            if time.monotonic() >= deadline:
                self.fail('Timed out waiting for the expected transition')
            await asyncio.sleep(.01)

    async def test_uart_recovery_sends_new_stop_requires_dual_ack_and_never_resumes_motion(self):
        self.controller._request('move', (2, 35, 1, 1000, 2000))
        await self.until(lambda: any(p[1] == 2 for p in self.client.writes))
        self.client.online = False
        await self.until(lambda: self.controller.stats['offline'] == 1)
        await self.until(lambda: self.controller.held is None)
        self.assertTrue(self.client.is_connected)
        self.assertFalse(self.session.done())
        self.assertFalse(self.controller.available())
        offline_seq = self.controller.channel.stop_seq
        after_offline = len(self.client.writes)

        # Keep STM ACK behind ESP ACK to prove that recovering online alone is
        # insufficient to unlock movement or resume the old timed run.
        self.client.auto_stm_ack = False
        self.client.online = True
        await self.until(lambda: self.controller.stats['recovered'] == 1)
        await self.until(lambda: self.client.esp_ack == self.controller.channel.stop_seq)
        self.assertNotEqual(self.controller.channel.stop_seq, offline_seq)
        self.assertTrue(self.controller.channel.barrier)
        self.assertFalse(self.controller.available())
        self.assertIsNone(self.controller.held)

        self.client.acknowledge_stm_latest()
        await self.until(self.controller.available)
        await asyncio.sleep(.15)
        self.assertTrue(self.client.is_connected)
        self.assertIsNone(self.controller.held)
        self.assertTrue(all(p[1] == 0 for p in self.client.writes[after_offline:]))

    async def test_offline_stop_retry_is_periodic_and_does_not_enqueue_movement(self):
        self.client.online = False
        await self.until(lambda: self.controller.stats['offline'] == 1)
        after_offline = len(self.client.writes)
        self.controller._request('move', (1, 60, 0, 0, 0))
        await self.until(lambda: self.controller.stats['retries'] >= 2, timeout=3.2)
        retries = self.client.writes[after_offline:]
        self.assertGreaterEqual(len(retries), 2)
        self.assertTrue(all(packet[1] == 0 for packet in retries))
        self.assertTrue(self.client.is_connected)
        self.assertIsNone(self.controller.held)
        self.assertFalse(self.controller.available())
        stop_sequences = [struct.unpack_from('<I', packet, 4)[0] for packet in retries]
        self.assertEqual(len(stop_sequences), len(set(stop_sequences)))

    async def test_manual_resync_requires_a_fresh_stop_confirmation(self):
        previous_stop = self.controller.channel.stop_seq
        self.client.auto_stm_ack = False
        self.controller._request('resync', ())
        await self.until(lambda: self.client.esp_ack == self.controller.channel.stop_seq)
        self.assertNotEqual(self.controller.channel.stop_seq, previous_stop)
        self.assertTrue(self.controller.channel.barrier)
        self.assertFalse(self.controller.available())
        self.client.acknowledge_stm_latest()
        await self.until(self.controller.available)
        self.assertIsNone(self.controller.held)
        self.assertEqual(self.client.writes[-1][1], 0)


class LinkDescriptionTests(unittest.TestCase):
    def description(self, **kwargs):
        return link_description(parse_status(status_bytes(**kwargs)))

    def test_ble_ok_uart_disabled_is_distinct_from_stm_offline(self):
        title, detail = self.description(uart=False)
        self.assertIn('BLE 正常', title)
        self.assertIn('串口未启用', title)
        self.assertIn('S3 UART', detail)

    def test_stm_offline_retains_ble_ok_and_has_uart_diagnostics(self):
        title, detail = self.description(online=False)
        self.assertIn('BLE 正常', title)
        self.assertIn('无有效串口回包', title)
        self.assertIn('115200', detail)
        self.assertNotIn('蓝牙断开', title)

    def test_stm_fault_and_local_busy_are_separate(self):
        self.assertIn('控制链路异常', self.description(fault=True)[0])
        self.assertIn('本地动作占用', self.description(motion=5)[0])

    def test_peer_not_ready_and_online_ready_are_separate(self):
        self.assertIn('未就绪', self.description(ready=False)[0])
        title, detail = self.description()
        self.assertIn('串口在线', title)
        self.assertIn('不会自动恢复运动', detail)


if __name__ == '__main__':
    unittest.main(verbosity=2)
