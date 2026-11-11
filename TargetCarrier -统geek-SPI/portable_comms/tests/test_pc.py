import unittest,sys,struct,zlib
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'pc'))
from tc_protocol import *
class ProtocolTests(unittest.TestCase):
 def test_lengths(self):
  for n in range(513):
   data=bytes(i&255 for i in range(n));j=Join();joined=None
   for b in pack(STATUS,3,n,data):joined=j.feed(unpack(b),0)
   self.assertEqual(joined,data)
 def test_resync(self):
  p=Parser();b=pack(READ,4,2,b'xx')[0];self.assertEqual(p.feed(b'garbage'+b[:100]),[]);self.assertEqual(p.feed(b[100:])[0]['data'],b'xx')
 def test_corruption(self):
  b=pack(READ,4,1,b'ab')[0]
  for i in range(128):
   c=bytearray(b);c[i]^=1
   with self.assertRaises(ValueError):unpack(c)
 def test_order_timeout(self):
  a,b=map(unpack,pack(STATUS,4,5,bytes(100)));j=Join();self.assertIsNone(j.feed(b,0));self.assertIsNone(j.feed(a,0));self.assertIsNone(j.feed(b,.301))
 def test_layout(self):
  b=bytearray(pack(READ,4,1)[0]);b[22]=1;struct.pack_into('<I',b,124,zlib.crc32(b[:124]))
  with self.assertRaises(ValueError):unpack(b)
 def test_bound(self):
  with self.assertRaises(ValueError):pack(READ,4,1,bytes(513))
  c=Client();c.connected=True
  for _ in range(8):self.assertTrue(c.request(READ,b'aa'))
  self.assertFalse(c.request(READ,b'aa'))
 def test_wave(self):
  d=struct.pack('<BBBBIIHHII',1,2,1,1,1234,40000,1,2,3,4);w=wave_decode(d);self.assertEqual(w['values'],(3,4))
  with self.assertRaises(ValueError):wave_decode(d[:-1])
 def test_c_golden(self):
  f=unpack(Path('golden.bin').read_bytes());self.assertEqual(f['kind'],WAVE);self.assertEqual(wave_decode(f['data'])['values'],(3,4))
if __name__=='__main__':unittest.main(verbosity=2)
