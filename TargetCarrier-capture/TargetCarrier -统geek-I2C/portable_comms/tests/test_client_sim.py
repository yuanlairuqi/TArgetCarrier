"""Run the production Python client against the production C parameter service.
The serial/RF link and time are simulated; this is not a hardware benchmark.
"""
import unittest, sys, ctypes, struct, types, queue
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'pc'))
import tc_protocol as tc
lib = ctypes.CDLL(str(Path('sim_service.dll').resolve()))
lib.sim_receive.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
lib.sim_pop.argtypes = [ctypes.c_void_p, ctypes.c_uint32]

class Clock:
    def __init__(self): self.now = 0
    def monotonic(self): return self.now

class Event:
    def __init__(self, clock, end, hook): self.clock=clock; self.end=end; self.hook=hook; self.flag=False
    def is_set(self): return self.flag or self.clock.now >= self.end
    def set(self): self.flag=True
    def wait(self, seconds): self.clock.now += seconds; self.hook(self.clock.now)

class SimPort:
    def __init__(self, clock, drop=False, blackhole=False):
        self.clock=clock; self.drop=drop; self.blackhole=blackhole; self.sent=[]; self.rx=bytearray(); self.dropped=False
    def __enter__(self): return self
    def __exit__(self,*args): pass
    def reset_input_buffer(self): pass
    def write(self,b):
        f=tc.unpack(b); self.sent.append(f)
        if self.blackhole: return len(b)
        lib.sim_receive(ctypes.create_string_buffer(b),int(self.clock.now*1000))
        slot=ctypes.create_string_buffer(128)
        while lib.sim_pop(slot,int(self.clock.now*1000)):
            if self.drop and f['kind']==tc.WRITE and not self.dropped:
                self.dropped=True
                # Inject a valid but late unrelated result into the same session.
                self.rx.extend(tc.pack(tc.RESULT,f['session'],f['seq']-1,bytes([tc.WRITE,0,1,0,1,0,0,1,99,0,0,0]))[0])
            else: self.rx.extend(slot.raw)
        return len(b)
    def read(self,n): data=bytes(self.rx[:n]); del self.rx[:n]; return data

class ClientSimulation(unittest.TestCase):
    def run_sim(self,drop=False,blackhole=False,end=1.2):
        lib.sim_init();clock=Clock();c=tc.Client();c.port='SIM';port=SimPort(clock,drop,blackhole);requested=[]
        def hook(now):
            if now>=.05 and not requested:
                c.request(tc.WRITE,struct.pack('<HBBI',1,1,0,42));requested.append(True)
        c.stop_event=Event(clock,end,hook)
        with patch.dict(sys.modules,{'serial':types.SimpleNamespace(Serial=lambda *a,**k:port)}),patch.object(tc,'time',clock): c._run()
        return c,port
    def test_lost_reply_duplicate_and_late(self):
        c,p=self.run_sim(drop=True)
        self.assertEqual(lib.sim_value(),42)
        self.assertEqual(lib.sim_duplicates(),1)
        self.assertEqual(c.late,1)
        writes=[f for f in p.sent if f['kind']==tc.WRITE]
        self.assertEqual(len(writes),2)
        self.assertEqual(writes[0]['seq'],writes[1]['seq'])
        events=list(c.events.queue)
        self.assertTrue(any(e[0]=='result' and e[1][0]==tc.WRITE and e[1][7]==1 for e in events))
        self.assertTrue(c.commands.empty())
    def test_timeout_retries_bounded_and_pending_requests_cleared(self):
        c,p=self.run_sim(blackhole=True,end=1.45)
        first=p.sent[0]['session'];attempts=[f for f in p.sent if f['session']==first]
        self.assertEqual(len(attempts),4) # initial + 3 retries
        self.assertFalse(any(f['kind']==tc.WRITE for f in p.sent))
        self.assertTrue(c.commands.empty())
        self.assertTrue(any(e[0]=='error' for e in list(c.events.queue)))
    def test_control_handler_executes_once(self):
        lib.sim_init()
        for b in tc.pack(tc.HELLO,33,1): lib.sim_receive(ctypes.create_string_buffer(b),1)
        request=tc.pack(tc.CONTROL,33,2,struct.pack('<H',100))[0]
        lib.sim_receive(ctypes.create_string_buffer(request),2)
        lib.sim_receive(ctypes.create_string_buffer(request),3)
        self.assertEqual(lib.sim_executed(),1)
        self.assertEqual(lib.sim_duplicates(),1)
    def test_wave_events_do_not_block_results(self):
        c=tc.Client()
        for i in range(1000):c.event('wave',i)
        c.event('result',b'important')
        self.assertEqual(c.events.get_nowait(),('result',b'important'))
        self.assertEqual(c.waves.qsize(),64)
        self.assertEqual(c.drops,936)
if __name__=='__main__': unittest.main(verbosity=2)
