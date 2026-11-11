import queue,threading,unittest
from serial_hub import SerialHub
class CaptureRouting(unittest.TestCase):
 def setUp(self):
  self.h=SerialHub.__new__(SerialHub);self.h.lock=threading.RLock();self.h.mail={};self.h.capture_mail=set();self.h.capture_drops=0;self.h.drops=0
 def test_bounded_and_unsubscribe(self):
  q=self.h.capture_subscribe(1);self.h.route(4,bytes(64));self.h.route(4,bytes(64));self.assertEqual(self.h.capture_drops,1);self.assertEqual(q.get_nowait()[1],4);self.h.capture_unsubscribe(q);self.h.route(4,bytes(64));self.assertTrue(q.empty())
 def test_control_unaffected_by_capture_flood(self):
  control,wave=queue.Queue(32),queue.Queue(8);self.h.mail[1]=(control,wave);q=self.h.capture_subscribe(1);b=bytearray(64);b[4]=1
  for _ in range(200):self.h.route(4,b)
  self.h.route(2,b);self.assertEqual(control.get_nowait()[1],2);self.assertLessEqual(wave.qsize(),8);self.assertEqual(q.qsize(),1)
 def test_bad_capacity(self):
  with self.assertRaises(ValueError):self.h.capture_subscribe(0)
if __name__=='__main__':unittest.main()
