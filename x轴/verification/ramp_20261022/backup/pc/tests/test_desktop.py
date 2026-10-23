import json
import math
import queue
import struct
import sys
import time
import unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from desktop import Bridge
from protocol import parse_status

class Recorder:
    def __init__(self):self.calls=[]
    def call(self,*args):self.calls.append(args)

class DesktopTests(unittest.TestCase):
    def setUp(self):
        self.controller=Recorder();self.b=Bridge(self.controller)
    def ready(self):
        self.b._state['available']=True
        self.b.refresh(True)
    def test_no_movement_until_ready_and_focused(self):
        with self.assertRaises(ValueError):self.b.move(1,40,0,0,0)
        self.b._state['available']=True
        with self.assertRaises(ValueError):self.b.move(1,40,0,0,0)
        self.assertFalse(any(c[0]=='move' for c in self.controller.calls))
    def test_invalid_bridge_commands_do_not_reach_controller(self):
        self.ready()
        for values in [(1,0,0,0,0),(True,40,0,0,0),(1,math.nan,0,0,0),(1,40,1,99,100),(1,40,0,1.5,0)]:
            with self.assertRaises(ValueError):self.b.move(*values)
        with self.assertRaises(ValueError):self.b.action('connect','not-discovered')
        with self.assertRaises(ValueError):self.b.action('shell')
    def test_timed_command_and_focus_loss_stop(self):
        self.ready();self.assertTrue(self.b.move(2,35,1,1000,2000))
        self.assertEqual(self.controller.calls[-1],('move',2,35,1,1000,2000))
        result=self.b.refresh(False)
        self.assertFalse(result['running']);self.assertEqual(self.controller.calls[-1],('stop',))
    def test_refresh_without_focus_never_renews_ui_lease(self):
        self.b.refresh(False);self.assertNotIn(('alive',),self.controller.calls)
    def test_backend_disconnect_locks_display(self):
        self.b._events.put(('disconnected',None));self.b._state.update(running=True,available=True)
        state=self.b.refresh(True)
        self.assertFalse(state['available']);self.assertFalse(state['running']);self.assertIsNone(state['status'])
    def test_diagnostics_and_frozen_exports(self):
        self.b._samples.append(dict(at=1,speed=20))
        self.b.freeze('status',True)
        self.b._samples.append(dict(at=2,speed=70))
        self.assertIn('20',self.b._export_text('status'))
        self.assertNotIn('70',self.b._export_text('status'))
        self.b.freeze('status',False)
        self.assertIn('70',self.b._export_text('status'))
        self.assertEqual(json.loads(self.b._export_text('diagnostics'))['version'],'2.1')
    def test_snapshot_bounded_for_rendering_export_retains_history(self):
        for i in range(1000):self.b._samples.append(dict(at=i,speed=i%100))
        self.assertEqual(len(self.b.refresh(False)['samples']),320)
        self.assertEqual(len(self.b._export_text('status').splitlines()),1001)

if __name__=='__main__':unittest.main()
