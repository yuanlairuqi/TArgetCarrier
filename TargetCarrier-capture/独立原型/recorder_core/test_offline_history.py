import struct,tempfile,unittest,zlib,csv
from pathlib import Path
from offline_history import records,export
def record(source,seq,ts,bits=1):
 b=bytearray(40);b[:3]=b'CR\x01';b[3]=1;b[4]=1;struct.pack_into('<HH',b,6,5,7);struct.pack_into('<III',b,12,source,1,seq);struct.pack_into('<Q',b,24,ts);struct.pack_into('<I',b,32,bits);struct.pack_into('<I',b,36,zlib.crc32(b[:36]));return bytes(b)
class TestHistory(unittest.TestCase):
 def test_filter_export_and_preserve_original(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'original';raw=record(1,0,100)+record(2,1,200);p.write_bytes(raw);o=Path(d)/'export.csv';export(p,o,2,150,250);rows=list(csv.DictReader(o.read_text(encoding='utf-8-sig').splitlines()));self.assertEqual(len(rows),1);self.assertEqual(rows[0]['source'],'2');self.assertEqual(p.read_bytes(),raw)
   with self.assertRaises(FileExistsError):export(p,o)
   with self.assertRaises(ValueError):export(p,p)
 def test_corrupt_or_truncated(self):
  with tempfile.TemporaryDirectory() as d:
   p=Path(d)/'input';p.write_bytes(record(1,1,5)[:-1]);
   with self.assertRaises(ValueError):list(records(p))
   b=bytearray(record(1,1,5));b[25]^=1;p.write_bytes(b)
   with self.assertRaises(ValueError):list(records(p))
if __name__=='__main__':unittest.main()
