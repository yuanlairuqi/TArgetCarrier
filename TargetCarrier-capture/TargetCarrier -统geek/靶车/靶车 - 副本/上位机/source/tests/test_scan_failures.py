import asyncio
import queue
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from usb_transport import CombinedController

class ScanTests(unittest.IsolatedAsyncioTestCase):
    def controller(self,scanner):
        c=CombinedController.__new__(CombinedController)
        c.events=queue.Queue();c.scanner=scanner;c.devices={'stale':SimpleNamespace(name='TC-XAxis')}
        return c

    async def test_failed_ble_scan_clears_stale_devices_and_preserves_failure_with_usb(self):
        class Scanner:
            @staticmethod
            async def discover(**kwargs):raise RuntimeError('POWERED_OFF')
        c=self.controller(Scanner)
        with patch('serial.tools.list_ports.comports',return_value=[SimpleNamespace(device='COM9',description='USB probe')]):
            await c.scan()
        self.assertNotIn('stale',c.devices);self.assertIn('usb:COM9',c.devices)
        events=list(c.events.queue)
        messages=[v for k,v in events if k=='connection']
        self.assertIn('电脑蓝牙已关闭',messages[-1])
        self.assertEqual([v for k,v in events if k=='devices'][0],[])
        self.assertEqual([v for k,v in events if k=='busy'][-1],False)

    async def test_cancelled_scan_clears_candidates_and_releases_busy(self):
        entered=asyncio.Event()
        class Scanner:
            @staticmethod
            async def discover(**kwargs):entered.set();await asyncio.sleep(60)
        c=self.controller(Scanner)
        task=asyncio.create_task(c.scan());await entered.wait();task.cancel()
        with self.assertRaises(asyncio.CancelledError):await task
        self.assertFalse(c.devices)
        self.assertEqual([v for k,v in c.events.queue if k=='busy'][-1],False)
