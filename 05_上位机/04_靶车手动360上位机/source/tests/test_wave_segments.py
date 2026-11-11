import unittest,tempfile,struct,csv
from pathlib import Path
from test_wave_recording import fragments,crc
from wave_history import rows,frames,header,ordered_paths,export_csv

def write(path,parts,version=2,session=99,segment=0,ident=None,origin=0):
 ident=segment+1 if ident is None else ident
 p=b'TCWL'+bytes([version,0,0,0])+struct.pack('<QIQ',100000,ident,origin)
 if version==2:p+=struct.pack('<QII',session,segment,0)
 p=crc(p)
 for i,b in parts:p+=crc(struct.pack('<Q',i*40000+10000)+b+struct.pack('<I',0))
 path.write_bytes(p)
class Segments(unittest.TestCase):
 def setup_files(self,t,version=2):
  a,b=Path(t)/'a.twl',Path(t)/'b.twl';fs=fragments()
  write(a,list(enumerate(fs[:2])),version=version,segment=0)
  write(b,list(enumerate(fs[2:],start=2)),version=version,segment=1)
  return a,b
 def test_v2_cross_segment_reverse_selection_and_export(self):
  with tempfile.TemporaryDirectory() as t:
   a,b=self.setup_files(t);out=list(rows([b,a]));self.assertEqual([x['value'] for x in out],[123]);self.assertEqual(out[0]['seq'],1)
   csvfile=Path(t)/'out.csv';export_csv([b,a],csvfile)
   with csvfile.open(encoding='utf-8-sig') as f:self.assertEqual(len(list(csv.DictReader(f))),1)
 def test_v1_explicit_cross_segment(self):
  with tempfile.TemporaryDirectory() as t:
   a,b=self.setup_files(t,1);self.assertEqual(len(list(rows([b,a]))),1)
 def test_missing_duplicate_and_mixed_sessions_rejected(self):
  with tempfile.TemporaryDirectory() as t:
   a,b=self.setup_files(t)
   with self.assertRaises(ValueError):ordered_paths([a,a])
   for changes in [dict(segment=2),dict(segment=0),dict(segment=1,session=100),dict(segment=1,version=1),dict(segment=1,origin=42)]:
    write(b,[],**changes)
    with self.assertRaises(ValueError):list(rows([a,b]))
 def test_legacy_mixed_clock_rejected(self):
  with tempfile.TemporaryDirectory() as t:
   a,b=self.setup_files(t,1);write(b,[],version=1,segment=1,origin=999999)
   with self.assertRaises(ValueError):list(rows([a,b]))
 def test_interleaved_nodes_cross_boundary(self):
  from test_wave_recording import rec
  import struct
  allparts=[];one=fragments();two=[]
  for raw in fragments(rec(node=2)):
   p=bytearray(raw[:60]);struct.pack_into('<I',p,4,2);two.append(crc(p))
  for a,b in zip(one,two):allparts.extend([a,b])
  with tempfile.TemporaryDirectory() as t:
   a,b=Path(t)/'a',Path(t)/'b';write(a,list(enumerate(allparts[:3])),segment=0);write(b,list(enumerate(allparts[3:],start=3)),segment=1)
   self.assertEqual({x['node'] for x in rows([b,a])},{1,2});self.assertEqual(len(list(rows([b,a],node=1))),1)
 def test_timestamp_reversal_rejected(self):
  with tempfile.TemporaryDirectory() as t:
   a,b=self.setup_files(t);write(b,list(enumerate(fragments()[2:])),segment=1)
   with self.assertRaises(ValueError):list(rows([a,b]))
 def test_single_v1_v2_and_header_crc(self):
  with tempfile.TemporaryDirectory() as t:
   p=Path(t)/'p'
   for v in [1,2]:
    write(p,list(enumerate(fragments())),version=v);self.assertEqual(len(list(rows(p))),1);self.assertEqual(header(p)['size'],32 if v==1 else 48)
    raw=bytearray(p.read_bytes());raw[8]^=1;p.write_bytes(raw)
    with self.assertRaises(ValueError):list(frames(p))
