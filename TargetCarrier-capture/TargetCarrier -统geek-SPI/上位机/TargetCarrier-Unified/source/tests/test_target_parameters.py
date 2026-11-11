import math,struct,sys,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from target_parameters import TargetParameters,encode_value

class RegisterTests(unittest.TestCase):
    def setUp(self):
        self.now=10.;self.p=TargetParameters(lambda:self.now)
    def reply(self,rid,value,dtype='float'):
        return self.p.receive(dict(rid=rid,ok=1,raw=encode_value(dtype,value).hex()))
    def test_existing_frame_and_config_comparison(self):
        self.p.start('read_config',{})
        for rid,value,dtype in ((10,1,'uint'),(21,12.5,'float'),(22,45.,'float'),(23,10.,'float')):
            packet=self.p.next_packet(42)
            self.assertEqual(packet[2:6],bytes([4,42,1,rid]));self.assertEqual(len(packet),17)
            self.reply(rid,value,dtype)
        self.assertTrue(self.p.config_verified);self.assertFalse(self.p.busy)
        config_result=self.p.config_result
        self.p.start('read_parameter',dict(rid=6));self.p.next_packet(3);self.reply(6,40)
        self.assertEqual(self.p.snapshot()['config_result'],config_result)
    def test_mismatch_does_not_silently_change_mapping_or_confirm_config(self):
        self.p.start('read_config',{})
        for rid,value,dtype in ((10,1,'uint'),(21,12.5,'float'),(22,30.,'float'),(23,10.,'float')):
            self.p.next_packet(1);self.reply(rid,value,dtype)
        self.assertFalse(self.p.config_verified);self.assertIn('VMAX=30',self.p.result)
    def test_write_once_then_independent_readback(self):
        self.p.start('write_parameter',dict(rid=6,dtype='float',value=5))
        self.assertEqual(self.p.next_packet(1)[4],3)
        self.reply(6,5);self.assertTrue(self.p.busy);self.assertIn('等待独立读取',self.p.result)
        self.assertEqual(self.p.next_packet(2)[4],1);self.reply(6,5)
        self.assertFalse(self.p.busy);self.assertIn('回读一致',self.p.result)
    def test_write_timeout_never_replays_write(self):
        self.p.start('write_parameter',dict(rid=6,dtype='float',value=5));self.p.next_packet(1)
        self.now+=1.1;self.assertIsNone(self.p.next_packet(2));self.assertFalse(self.p.busy)
        self.assertIn('结果未确认',self.p.result)
    def test_read_retry_bounded_and_wrong_rid_does_not_complete(self):
        self.p.start('read_parameter',dict(rid=6));self.p.next_packet(1)
        self.assertFalse(self.reply(7,0))
        self.now+=1.1;self.assertEqual(self.p.next_packet(2)[4],1)
        self.now+=1.1;self.assertIsNone(self.p.next_packet(3));self.assertFalse(self.p.busy)
    def test_readback_mismatch_is_not_success(self):
        self.p.start('write_parameter',dict(rid=6,dtype='float',value=5));self.p.next_packet(1)
        self.reply(6,5);self.p.next_packet(2);self.reply(6,6)
        self.assertIn('回读不一致',self.p.result)
    def test_cancel_quarantines_old_responses_and_does_not_replay(self):
        self.p.start('read_config',{});self.p.next_packet(1);self.p.cancel('停止',invalidate=True)
        self.assertFalse(self.reply(10,1,'uint'));self.assertFalse(self.p.config_verified)
        self.assertIsNone(self.p.next_packet(2))
        with self.assertRaises(ValueError):self.p.start('read_config',{})
    def test_invalid_values_and_mapping_writes_rejected(self):
        for value in (math.nan,math.inf,1e100):
            with self.assertRaises(ValueError):encode_value('float',value)
        for value in (-1,1.5,2**32):
            with self.assertRaises(ValueError):encode_value('uint',value)
        for rid in (7,8,10,21,22,23):
            with self.assertRaises(ValueError):self.p.start('write_parameter',dict(rid=rid,value=1))
        for rid,value in ((6,0),(6,-1),(3,2),(2,79),(24,0)):
            with self.assertRaises(ValueError):self.p.start('write_parameter',dict(rid=rid,dtype='float',value=value))
        with self.assertRaises(ValueError):self.p.start('write_parameter',dict(rid=6,dtype='uint',value=5))
    def test_nonfinite_reply_rejected(self):
        self.p.start('read_parameter',dict(rid=6));self.p.next_packet(1)
        self.p.receive(dict(rid=6,ok=1,raw=struct.pack('<f',math.nan).hex()))
        self.assertNotIn(6,self.p.values);self.assertFalse(self.p.busy)
