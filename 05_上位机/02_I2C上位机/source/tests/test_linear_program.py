"""Production bridge + production HTTP owner, injected clock/transport only."""
import copy
import sys
import unittest
import time
import queue
from pathlib import Path
from urllib.parse import unquote
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from unified import UnifiedBridge
from target_adapter import TargetAdapter
from linear_program import validate_step
from protocol import parse_status
from test_ramp import status


class Clock:
    def __init__(self): self.now = 100.
    def __call__(self): return self.now


class Controller:
    def __init__(self): self.calls = []
    def call(self, *args): self.calls.append(args)


class Host:
    def __init__(self):
        self.calls = []
        self.reject_enable = False
        self.fail = False
        self.coasting = False
        self.state = dict(en=0, estop=1, up=100, yaw=0, pitch=0, kp=40, kd=1.5,
                          st_age=1, br_age=1, st_state=3, st_err=0, st_fresh=1,
                          st_online=1, enabled=0, arm=0, safe=0, m_vel=0,
                          vel=0, cruise=0, ry=0, rp=0)

    def __call__(self, endpoint, path):
        if self.fail: raise TimeoutError('injected link loss')
        if path == '/api/status':
            result = copy.deepcopy(self.state)
            if self.coasting: result['m_vel'] = .3
            return result
        cmd = unquote(path.split('c=', 1)[1])
        self.calls.append(cmd)
        s = self.state
        if cmd == 'x': s.update(estop=1, en=0, arm=0, enabled=0, st_state=3, m_vel=0, ry=0, rp=0)
        elif cmd == 'C': s.update(en=0, arm=0, enabled=0, m_vel=0)
        elif cmd == 'R': s.update(estop=0, st_state=0)
        elif cmd == '0': s.update(vel=0, cruise=0, ry=0, rp=0)
        elif cmd.startswith('V '):
            s['ry'] = float(cmd[2:])
            if s['ry']: s['vel'] = 1
        elif cmd.startswith('U '):
            s['rp'] = float(cmd[2:])
            if s['rp']: s['vel'] = 1
        elif cmd == 'E' and not self.reject_enable:
            s.update(en=1, arm=1, enabled=1, st_state=1)
        return {'ok': 1}


X = dict(kind='x', seconds=10, direction=1, speed=40, accel=200, decel=200)
T = dict(kind='target', seconds=10, yaw_rate=10, pitch_rate=0)
W = dict(kind='wait', seconds=1)


class ProgramTests(unittest.TestCase):
    def setUp(self):
        self.clock, self.c, self.host = Clock(), Controller(), Host()
        clock_patch = patch('time.monotonic', self.clock)
        clock_patch.start(); self.addCleanup(clock_patch.stop)
        self.a = TargetAdapter(self.host, self.clock, background=False)
        self.a.connect('127.0.0.1'); self.a.alive(True); self.a.tick()
        self.b = UnifiedBridge(self.c, self.a)
        for program in self.b._programs.values(): program.clock = self.clock
        self.x, self.t = self.b._programs['x'], self.b._programs['target']
        self.x_status()

    def x_status(self, seq=1, op=0, speed=0, motion=0, phase=0, hz=0):
        self.b._events.put(('status', parse_status(status(seq, op, speed, phase, hz, motion))))
        self.b._events.put(('available', True))
        self.b._last_focus = self.clock(); self.b._drain()

    def tick(self, dt=.15, focused=True):
        self.clock.now += dt; self.a.alive(focused); self.a.tick()
        self.b.refresh(focused)

    def advance(self, seconds):
        while seconds > 1e-8:
            dt = min(.15, seconds); self.tick(dt); seconds -= dt

    def add(self, step, lane=None):
        self.b.program_edit(lane or step['kind'], 'append', step)

    def start(self, lane):
        self.b._last_focus = self.clock()
        self.b.program_start(lane, self.b._programs[lane].generation)

    def start_both(self):
        self.b._last_focus = self.clock()
        self.b.program_start_all({key: p.generation for key,p in self.b._programs.items()})

    def run_x(self):
        self.tick(); self.assertEqual(self.x.phase, 'x_starting')
        self.x_status(2, 1, 40, 1, 1, 1); self.tick()
        self.assertEqual(self.x.phase, 'running')

    def run_target(self):
        for _ in range(12):
            self.tick()
            if self.t.phase == 'running': return
        self.fail(self.t.snapshot())

    def test_append_reverse_does_not_change_current_timer_or_send_early(self):
        self.add(X); self.start('x'); self.run_x(); self.advance(3)
        started = self.x.started
        self.add(dict(X, direction=2)); self.assertEqual(self.x.started, started)
        self.advance(6.5)
        self.assertFalse(any(c[:2] == ('move', 2) for c in self.c.calls))
        self.advance(.6); self.assertEqual(self.x.phase, 'x_stopping')
        self.assertIn(('soft_stop',), self.c.calls)
        self.advance(.3); self.assertEqual(self.x.phase, 'x_stopping')
        self.assertFalse(any(c[:2] == ('move', 2) for c in self.c.calls))
        self.x_status(3); self.tick(); self.tick()
        self.assertEqual(self.x.phase, 'x_starting')
        self.assertTrue(any(c[:2] == ('move', 2) for c in self.c.calls))

    def test_parallel_start_overlaps_x_and_target_motion(self):
        self.add(X); self.add(T); self.start_both(); self.run_x(); self.run_target()
        self.assertEqual(self.x.phase, 'running')
        self.assertEqual(self.t.phase, 'running')
        self.assertIn('V 10', self.host.calls)
        self.assertFalse(any(c[0] in ('stop','soft_stop') for c in self.c.calls))

    def test_start_second_lane_while_first_is_running(self):
        self.add(X); self.start('x'); self.run_x()
        self.add(T); self.start('target'); self.run_target()
        self.assertTrue(self.x.active); self.assertTrue(self.t.active)

    def test_x_can_start_while_target_has_pending_operation(self):
        self.add(T); self.start('target'); self.tick()
        self.assertTrue(self.a.snapshot()['busy'])
        self.add(X); self.start('x'); self.tick()
        self.assertEqual(self.x.phase, 'x_starting')

    def test_stop_x_does_not_stop_target_lane(self):
        self.add(X); self.add(T); self.start_both(); self.run_x(); self.run_target()
        self.b.program_stop('x'); self.tick()
        self.assertFalse(self.x.active); self.assertTrue(self.t.active)
        self.assertEqual(self.host.state['enabled'], 1)

    def test_stop_target_does_not_stop_x_lane(self):
        self.add(X); self.add(T); self.start_both(); self.run_x(); self.run_target()
        marker = len(self.c.calls)
        self.b.program_stop('target'); self.tick()
        self.assertFalse(self.t.active); self.assertTrue(self.x.active)
        self.assertFalse(any(c[0] == 'stop' for c in self.c.calls[marker:]))

    def test_x_completion_does_not_wait_for_long_target_step(self):
        self.add(dict(X, seconds=.1)); self.add(T); self.start_both(); self.run_x()
        self.run_target(); self.x_status(3); self.tick(); self.tick()
        self.assertEqual(self.x.phase, 'complete'); self.assertTrue(self.t.active)

    def test_pending_edit_remove_preserves_current(self):
        self.add(X); self.add(dict(X,direction=2)); self.add(W,'x')
        self.start('x'); self.run_x()
        self.b.program_edit('x','update',dict(X,direction=2,seconds=5),self.x.rows[1]['id'])
        self.b.program_edit('x','remove',None,self.x.rows[2]['id'])
        with self.assertRaises(ValueError): self.b.program_edit('x','update',W,self.x.rows[0]['id'])
        self.assertEqual(self.x.rows[0]['step']['seconds'],10)
        self.assertEqual(self.x.rows[1]['step']['seconds'],5)

    def test_each_lane_accepts_only_its_own_motion_or_wait(self):
        with self.assertRaises(ValueError): self.add(T,'x')
        with self.assertRaises(ValueError): self.add(X,'target')
        self.add(W,'x'); self.add(W,'target')
        self.assertEqual(len(self.x.rows),1); self.assertEqual(len(self.t.rows),1)

    def test_target_requires_disabled_and_zero_velocity_before_next(self):
        self.add(dict(T,seconds=.2)); self.add(W,'target'); self.start('target'); self.run_target()
        self.advance(.3); self.assertEqual(self.t.phase,'target_stopping')
        self.host.coasting=True; self.advance(.3)
        self.assertEqual(self.t.phase,'target_stopping')
        self.host.coasting=False; self.advance(.3)
        self.assertIn(self.t.phase,('next','running'))

    def test_target_next_step_releases_only_within_authorized_program(self):
        self.add(dict(T,seconds=.2)); self.add(dict(T,yaw_rate=-10))
        self.start('target'); self.run_target(); self.advance(.3)
        for _ in range(12):
            self.tick()
            if self.t.phase=='running' and self.t.current['step']['yaw_rate']==-10: break
        self.assertEqual(self.t.current['step']['yaw_rate'],-10)
        self.assertEqual(self.t.phase,'running'); self.assertEqual(self.host.calls.count('R'),2)

    def test_http_ok_without_motor_enable_never_starts_timer(self):
        self.host.reject_enable=True; self.add(T); self.start('target'); self.advance(5.9)
        self.assertFalse(self.t.active); self.assertEqual(self.t.phase,'aborted')
        self.assertNotIn('V 10',self.host.calls)

    def test_target_link_fault_cancels_both_and_recovery_never_replays(self):
        self.add(X); self.add(T); self.start_both(); self.run_x(); self.run_target()
        self.host.fail=True; self.advance(.3)
        self.assertFalse(self.x.active); self.assertFalse(self.t.active)
        marker=len(self.c.calls); target_marker=len(self.host.calls)
        self.host.fail=False; self.advance(2)
        self.assertFalse(any(c[0]=='move' for c in self.c.calls[marker:]))
        self.assertNotIn('E',self.host.calls[target_marker:])

    def test_x_session_reset_cancels_both_even_after_recovery(self):
        self.add(X); self.add(T); self.start_both(); self.run_x(); self.run_target()
        self.b._events.put(('reset',None)); self.x_status(3); self.tick()
        self.assertFalse(self.x.active); self.assertFalse(self.t.active)

    def test_all_stop_invalidates_both_queues(self):
        for step in (X,dict(X,direction=2),T,dict(T,yaw_rate=-10)): self.add(step)
        self.start_both(); self.run_x(); self.run_target(); self.b.stop_all()
        for p in (self.x,self.t): self.assertEqual([r['state'] for r in p.rows],['aborted','cancelled'])
        self.assertIn(('stop',),self.c.calls); self.assertTrue(self.a.stop_pending)

    def test_focus_loss_during_wait_cancels_both(self):
        self.add(W,'x'); self.add(W,'target'); self.start_both(); self.tick(); self.tick(focused=False)
        self.assertFalse(self.x.active); self.assertFalse(self.t.active)

    def test_missing_ui_heartbeat_cancels_both_on_return(self):
        self.add(W,'x'); self.add(W,'target'); self.start_both(); self.tick(); self.tick(dt=.8)
        self.assertFalse(self.x.active); self.assertFalse(self.t.active)

    def test_stop_timeout_never_starts_next_and_stops_other_lane(self):
        self.add(dict(X,seconds=.1)); self.add(dict(X,direction=2)); self.add(T)
        self.start_both(); self.run_x(); self.advance(5.9)
        self.assertFalse(self.x.active); self.assertFalse(self.t.active)
        self.assertFalse(any(c[:2]==('move',2) for c in self.c.calls))

    def test_old_target_schema_does_not_block_independent_x_program(self):
        self.a.status.pop('enabled'); self.add(T)
        with self.assertRaises(ValueError): self.start('target')
        self.add(X); self.start('x'); self.assertTrue(self.x.active)

    def test_parallel_start_is_atomic_when_target_preflight_fails(self):
        self.add(X); self.add(T); self.a.status.pop('enabled')
        with self.assertRaises(ValueError): self.start_both()
        self.assertFalse(self.x.active); self.assertFalse(self.t.active)
        self.assertFalse(any(c[0]=='move' for c in self.c.calls))

    def test_wait_does_not_adopt_manually_running_axis(self):
        self.add(W,'x'); self.b._state['running']=True
        with self.assertRaises(ValueError): self.start('x')

    def test_manual_motion_cannot_override_same_program_lane(self):
        self.add(W,'x'); self.add(W,'target'); self.start_both()
        with self.assertRaises(ValueError): self.b.move(1,40,0,0,0)
        with self.assertRaises(ValueError): self.b.update_profile(80,200,200)
        with self.assertRaises(ValueError): self.b.target_command('enable')
        with self.assertRaises(ValueError): self.b.action('connect','usb:any')

    def test_complete_then_append_does_not_autostart(self):
        self.add(dict(W,seconds=.1),'x'); self.start('x'); self.advance(.6)
        self.assertEqual(self.x.phase,'complete'); self.add(X); self.advance(.3)
        self.assertFalse(self.x.active); self.assertFalse(any(c[0]=='move' for c in self.c.calls))

    def test_snapshot_is_detached_from_live_rows(self):
        self.add(X); self.x.snapshot()['rows'][0]['step']['seconds']=.1
        self.assertEqual(self.x.rows[0]['step']['seconds'],10)

    def test_delayed_start_after_all_stop_is_invalidated(self):
        self.add(W,'x'); generation=self.x.generation; self.b.stop_all()
        with self.assertRaises(ValueError): self.b.program_start('x',generation)
        self.assertFalse(self.x.active)

    def test_stopping_one_lane_invalidates_only_its_delayed_start(self):
        self.add(W,'x'); self.add(W,'target')
        xgen,tgen=self.x.generation,self.t.generation; self.b.program_stop('x')
        with self.assertRaises(ValueError): self.b.program_start('x',xgen)
        self.b.program_start('target',tgen); self.assertTrue(self.t.active)

    def test_invalid_lane_and_generation_rejected(self):
        self.add(W,'x')
        for lane,gen in [('bad',0),('x',None),('x',True),('x',999)]:
            with self.subTest(lane=lane,gen=gen),self.assertRaises(ValueError):self.b.program_start(lane,gen)

    def test_bad_parameters_and_overflow_are_rejected(self):
        for value in (dict(X,seconds=float('nan')),dict(X,direction=True),dict(X,speed=101),
                      dict(T,yaw_rate=91),dict(T,pitch_rate=61),dict(T,yaw_rate=0),dict(W,seconds=0)):
            with self.subTest(value=value),self.assertRaises(ValueError):validate_step(value)
        for _ in range(self.x.LIMIT):self.add(W,'x')
        with self.assertRaises(ValueError):self.add(W,'x')


class OwnerIntegrationTests(unittest.TestCase):
    def test_production_owner_confirms_stop_before_appended_reverse(self):
        from controller import Controller as RealController
        from test_ramp import ProfileClient
        class Client(ProfileClient):
            async def write_gatt_char(self, tx, data, response=True):
                writes.append((time.monotonic(), data[1]))
                if data[1] in (1, 2): self.soft_at = None
                await super().write_gatt_char(tx, data, response)
        writes = []
        c = RealController(queue.Queue(), client_factory=Client)
        c.ramp = True
        c.devices = {'fake': None}
        b = UnifiedBridge(c, TargetAdapter(background=False))
        c.events = b._events
        c.call('connect', 'fake')
        def until(predicate, seconds=6):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                result = b.refresh(True)
                if predicate(result): return result
                time.sleep(.03)
            self.fail(b._programs['x'].snapshot())
        try:
            until(lambda s: s['available'])
            b.program_edit('x', 'append', dict(X, seconds=.1))
            b.program_start('x', b._programs['x'].generation)
            until(lambda s: s['programs']['x']['phase'] == 'running')
            b.program_edit('x', 'append', dict(X, direction=2, seconds=.1))
            until(lambda s: s['programs']['x']['phase'] == 'complete')
            opcodes = [op for _, op in writes]
            first = opcodes.index(1)
            reverse = opcodes.index(2)
            self.assertIn(3, opcodes[first:reverse])  # deceleration request
            self.assertIn(0, opcodes[first:reverse])  # real owner STOP barrier
            self.assertGreater(writes[reverse][0] - writes[first][0], 1.4)
        finally:
            c.call('disconnect')
            deadline = time.monotonic() + 2
            while c.session and time.monotonic() < deadline: time.sleep(.03)
            c.loop.call_soon_threadsafe(c.loop.stop)
            c.thread.join(timeout=2)
            c.loop.close()


if __name__ == '__main__': unittest.main()
