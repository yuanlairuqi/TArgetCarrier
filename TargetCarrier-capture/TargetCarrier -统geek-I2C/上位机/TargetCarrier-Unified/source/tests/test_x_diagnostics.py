import sys,struct,unittest
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from x_diagnostics import parse_diagnostic
class DiagnosticTests(unittest.TestCase):
 def test_fields_and_invalid_frames(self):
  b=bytes([1,7,7,1,33,3,0,0])+struct.pack('<IIHBB',1000,120,100,2,1)
  d=parse_diagnostic(b);self.assertEqual((d['stm_error'],d['loop_max_ms'],d['uart_age_ms']),(1,120,100));self.assertTrue(d['historical'])
  for p in [b[:-1],bytes([2])+b[1:],b[:3]+bytes([5])+b[4:]]:
   with self.assertRaises(ValueError):parse_diagnostic(p)
