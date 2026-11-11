import copy
import unittest
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from urllib.parse import unquote

from target_adapter import TargetAdapter, endpoint_url, validate_status
from unified import UnifiedBridge


class Clock:
    def __init__(self): self.now = 10.
    def __call__(self): return self.now
    def advance(self, dt=.3): self.now += dt


class Host:
    def __init__(self):
        self.calls = []
        self.fail = False
        self.callback = None
        self.state = dict(en=0, estop=0, up=100, yaw=0., pitch=0., st_age=10,
                          st_state=1, st_err=0, kp=40, kd=1.5, st_yaw=0.)
    def __call__(self, endpoint, path):
        cmd = unquote(path.split('c=',1)[1]) if '?c=' in path else 'status'
        self.calls.append(cmd)
        if self.callback: self.callback(cmd)
        if self.fail: raise TimeoutError('injected timeout')
        if cmd == 'status': return copy.deepcopy(self.state)
        if cmd == 'x': self.state['estop'] = 1
        if cmd == 'C': self.state['en'] = 0
        if cmd == 'R': self.state['estop'] = 0
        if cmd == 'E': self.state['en'] = 1
        return {'ok':1}


class TargetTests(unittest.TestCase):
    def setUp(self):
        self.clock, self.host = Clock(), Host()
        self.a = TargetAdapter(self.host, self.clock, background=False)
        self.a.alive(True)
    def connected(self):
        self.a.connect('192.168.1.20')
        self.a.tick()
    def released(self):
        self.connected()
        self.a.command('release')
        self.a.tick()
    def test_no_network_on_construction(self):
        self.a.tick()
        self.assertEqual(self.host.calls, [])
    def test_endpoint_validation(self):
        self.assertEqual(endpoint_url('192.168.1.2:8080/'),'http://192.168.1.2:8080')
        for address in ('https://a','http://a/api','http://user@a','http://a?q=1','http://a:99999','a b',''):
            with self.subTest(address=address), self.assertRaises(ValueError): endpoint_url(address)
    def test_connect_requests_stop_only(self):
        self.connected()
        self.assertEqual(self.host.calls, ['x','C','status'])
        self.assertTrue(self.a.snapshot()['locked'])
        self.assertFalse(self.a.snapshot()['physical_stop_confirmed'])
        self.assertIsNone(self.a.snapshot()['stm_online'])
    def test_release_does_not_enable(self):
        self.released()
        self.assertNotIn('E', self.host.calls)
        self.assertFalse(self.a.snapshot()['locked'])
        self.assertEqual(self.host.state['en'], 0)
    def test_explicit_enable_only(self):
        self.connected()
        with self.assertRaises(ValueError): self.a.command('enable')
        self.a.command('release');self.a.tick()
        self.a.command('enable');self.a.tick()
        self.assertEqual(self.host.calls.count('E'),1)
    def test_stop_discards_pending_enable(self):
        self.released()
        self.a.command('enable')
        self.a.stop()
        self.a.tick()
        self.assertNotIn('E',self.host.calls)
        self.assertEqual(self.a.snapshot()['pending'],0)
    def test_expired_operation_dropped(self):
        self.released()
        self.a.command('enable')
        self.clock.advance(.65); self.a.alive(True)
        self.a.tick()
        self.assertNotIn('E',self.host.calls)
        self.assertTrue(self.a.snapshot()['locked'])
    def test_focus_loss_discards_pending(self):
        self.released();self.a.command('enable');self.a.alive(False);self.a.tick()
        self.assertNotIn('E',self.host.calls)
    def test_ui_lease_stops_active_host(self):
        self.released();self.a.command('enable');self.a.tick()
        self.clock.advance(.8);self.a.tick()
        self.assertEqual(self.host.calls[-3:],['x','C','status'])
        self.assertTrue(self.a.snapshot()['locked'])
    def test_failed_command_not_replayed_on_recovery(self):
        self.released();self.a.command('enable');self.host.fail=True;self.a.tick()
        self.assertEqual(self.host.calls.count('E'),1)
        self.host.fail=False
        for _ in range(4):
            self.clock.advance(.9);self.a.alive(True);self.a.tick()
        self.assertEqual(self.host.calls.count('E'),1)
        self.assertEqual(self.host.state['en'],0)
        self.assertTrue(self.a.snapshot()['locked'])
    def test_reboot_invalidates_session(self):
        self.released();self.a.command('enable');self.a.tick()
        self.host.state['up']=1
        self.clock.advance();self.a.tick()
        self.assertTrue(self.a.snapshot()['locked'])
        self.a.tick();self.assertEqual(self.host.state['en'],0)
    def test_old_status_marked_historical(self):
        self.connected();self.clock.advance(1.1)
        state=self.a.snapshot()
        self.assertTrue(state['historical']);self.assertFalse(state['connected'])
        self.assertIsNotNone(state['status'])
    def test_operation_ranges_and_injection(self):
        self.released()
        for name, args in [('angles',{'yaw':181,'pitch':0}),('gains',{'kp':40,'kd':float('nan')}),('profile',{'profile':6}),('velocity',{'yaw_rate':'1\nx','pitch_rate':0})]:
            with self.subTest(name=name), self.assertRaises(ValueError):self.a.command(name,args)
        with self.assertRaises(ValueError):self.a.command('J secret')
    def test_http_success_not_execution_confirmation(self):
        self.released();self.a.command('enable');self.a.tick()
        self.assertFalse(self.a.snapshot()['physical_stop_confirmed'])
        self.assertTrue(any('未确认' in l['text'] for l in self.a.snapshot()['logs']))
    def test_stop_attempts_disable_when_estop_fails(self):
        self.a.connect('192.168.1.2')
        def fail_x(cmd):
            if cmd=='x':raise TimeoutError('x failed')
        self.host.callback=fail_x;self.a.tick()
        self.assertIn('C',self.host.calls)
        self.assertFalse(self.a.snapshot()['connected'])
    def test_stop_interrupts_multi_command_batch(self):
        self.released()
        self.a.command('angles',dict(yaw=30,pitch=20))
        def interrupt(cmd):
            if cmd=='y 30':self.a.stop()
        self.host.callback=interrupt;self.a.tick();self.a.tick()
        self.assertIn('y 30',self.host.calls)
        self.assertNotIn('p 20',self.host.calls)
        self.assertIn('C',self.host.calls)
    def test_pending_is_bounded_and_inflight_busy(self):
        self.released();self.a.command('enable')
        with self.assertRaises(ValueError):self.a.command('enable')
        def check_busy(cmd):
            if cmd=='E':
                self.assertTrue(self.a.snapshot()['busy'])
                with self.assertRaises(ValueError):self.a.command('enable')
        self.host.callback=check_busy;self.a.tick()
    def test_stale_stm_blocks_enable(self):
        self.released();self.a.status['st_age']=1000
        with self.assertRaises(ValueError):self.a.command('enable')
    def test_bad_status_does_not_mark_online(self):
        for status in ({},dict(self.host.state,up=float('nan')),dict(self.host.state,en=2)):
            with self.assertRaises(ValueError):validate_status(status)
    def test_disconnect_does_not_reconnect(self):
        self.connected();self.a.command('disconnect');self.a.tick()
        calls=list(self.host.calls)
        self.clock.advance(5);self.a.tick()
        self.assertEqual(calls,self.host.calls)
        self.assertIsNone(self.a.snapshot()['endpoint'])
    def test_cannot_switch_endpoint_with_session(self):
        self.connected()
        with self.assertRaises(ValueError):self.a.connect('192.168.1.3')
    def test_repeated_forwarded_data_does_not_prove_stm_online(self):
        self.connected()
        for _ in range(10):
            self.clock.advance();self.a.tick()
        self.assertIsNone(self.a.snapshot()['stm_online'])
        self.assertFalse(self.a.snapshot()['physical_stop_confirmed'])


class FakeController:
    def __init__(self):self.calls=[]
    def call(self,*args):self.calls.append(args)


class BridgeTests(unittest.TestCase):
    def setUp(self):
        self.controller=FakeController()
        self.target=TargetAdapter(background=False)
        self.b=UnifiedBridge(controller=self.controller,target=self.target)
    def test_refresh_contains_two_independent_states(self):
        value=self.b.refresh(True)
        self.assertIn('target',value)
        self.assertFalse(value['unified']['automatic_joint_motion'])
        self.assertIn(('alive',),self.controller.calls)
    def test_global_stop_reaches_both_owners(self):
        self.target.connect('192.168.1.2')
        result=self.b.stop_all()
        self.assertTrue(result['x_axis_requested'])
        self.assertTrue(result['target_requested'])
        self.assertIn(('stop',),self.controller.calls)
        self.assertTrue(self.target.stop_pending)
    def test_global_stop_survives_x_owner_exception(self):
        def fail(*args):raise OSError('x owner failed')
        self.controller.call=fail
        result=self.b.stop_all()
        self.assertIn('x_axis_error',result)
        self.assertTrue(result['target_requested'])
    def test_shutdown_still_stops_x(self):
        self.b._shutdown()
        self.assertIn(('stop',),self.controller.calls)
        self.assertIn(('disconnect',),self.controller.calls)

    def test_all_stop_does_not_wait_for_target_network(self):
        import threading
        entered, release = threading.Event(), threading.Event()
        def blocked(endpoint, path):
            entered.set()
            release.wait(2)
            raise TimeoutError('blocked network')
        target=TargetAdapter(blocked)
        try:
            target.connect('192.168.1.2')
            self.assertTrue(entered.wait(1))
            self.b._target=target
            self.b.stop_all()
            self.assertIn(('stop',),self.controller.calls)
            self.assertFalse(release.is_set())
        finally:
            target.closed=True
            release.set()


if __name__=='__main__':unittest.main()
