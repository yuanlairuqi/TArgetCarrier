import unittest,tempfile,struct,zlib,time,sys,queue,threading
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0,str(Path(__file__).resolve().parents[1]))
from wave_history import Decoder,decode_record,frames,rows,export_csv
from wave_client import Recorder
from serial_hub import SerialHub

def crc(p):return p+struct.pack('<I',zlib.crc32(p))
def rec(seq=1,value=123,node=1,boot=7):
 p=bytearray(36);p[:5]=b'CR\1\1\1';struct.pack_into('<HH',p,6,1,1);struct.pack_into('<IIIQI',p,12,node,boot,seq,500000,value);return crc(p)
def fragments(data=None,ident=1,session=9):
 data=data or rec();out=[];count=(len(data)+11)//12
 for i in range(count):
  f=bytearray(20);n=min(12,len(data)-i*12);f[:4]=bytes([1,i,count,n]);struct.pack_into('<I',f,4,ident);f[8:8+n]=data[i*12:i*12+n]
  b=bytearray(60);b[:4]=b'WN\1\4';struct.pack_into('<IIII',b,4,1,7,3,session);b[32]=3;b[40:60]=f;out.append(crc(b))
 return out

def logfile(path,tail=b''):
 h=b'TCWL\1\0\0\0'+struct.pack('<QIQ',100000,1,10000);p=crc(h)
 for i,b in enumerate(fragments()):p+=crc(struct.pack('<Q',10000+i*40000)+b+struct.pack('<I',2))
 path.write_bytes(p+tail)

class DecodeTests(unittest.TestCase):
 def test_fragments(self):
  d=Decoder();result=None
  for i,b in enumerate(fragments()):result=d.feed(b,i*40)
  self.assertEqual(result['value'],123);self.assertEqual(result['us'],500000)
 def test_interleaved_gpio(self):
  d=Decoder();bs=fragments()
  for i,b in enumerate(bs):
   out=d.feed(b,i*40);pulse=bytearray(b[:60]);pulse[32]=1;self.assertIsNone(d.feed(crc(pulse),i*40+1))
  self.assertEqual(out['seq'],1)
 def test_corrupt_missing_timeout_session(self):
  for mode in ('corrupt','missing','timeout','session'):
   d=Decoder();bs=fragments()
   for i,b in enumerate(bs):
    if mode=='missing' and i==1:continue
    if mode=='corrupt' and i==1:b=b[:-1]+bytes([b[-1]^1])
    if mode=='session' and i==1:b=fragments(session=10)[i]
    out=d.feed(b,1000 if mode=='timeout' and i==3 else i*40)
   self.assertIsNone(out,mode)
 def test_duplicate_and_gaps(self):
  d=Decoder()
  for b in fragments():a=d.feed(b,0)
  for b in fragments():a=d.feed(b,10)
  self.assertIsNone(a)
  for b in fragments(rec(seq=4),ident=2):a=d.feed(b,20)
  self.assertEqual(d.gaps,2)
 def test_source_spoof_rejected(self):
  d=Decoder()
  for b in fragments(rec(node=2)):a=d.feed(b,0)
  self.assertIsNone(a)
 def test_raw_filter_export_and_power_loss(self):
  with tempfile.TemporaryDirectory() as t:
   p=Path(t)/'x.twl';logfile(p);data=list(rows(p));self.assertEqual(len(data),1);self.assertEqual(data[0]['sd_drops'],2)
   self.assertEqual(list(rows(p,node=2)),[]);self.assertEqual(list(rows(p,start=999999)),[])
   dest=Path(t)/'x.csv';export_csv(p,dest);self.assertIn('123',dest.read_text(encoding='utf-8-sig'))
   with self.assertRaises(FileExistsError):export_csv(p,dest)
   logfile(p,b'partial');self.assertEqual(len(list(frames(p,recovery=True))),4)
   with self.assertRaises(ValueError):list(frames(p))
 def test_invalid_header_and_nonfinite(self):
  data=bytearray(rec()[:36]);data[4]=3;data[32:36]=struct.pack('<f',float('nan'))
  with self.assertRaises(ValueError):decode_record(crc(data))

class Hub:
 def __init__(self):self.running=True;self.port='FAKE';self.capture_drops=0;self.mail=queue.Queue();self.writes=[]
 def capture_subscribe(self,n):return self.mail
 def capture_unsubscribe(self,q):pass
 def write(self,p,created):
  self.writes.append(p);q=p[4:68];b=bytearray(64);b[:4]=b'RS\1'+q[3:4];b[4:8]=q[4:8];b[9]=1
  if q[3] in (1,2,3,8):b[10]=24
  self.mail.put((time.monotonic(),7,bytes(b)));return len(p)
class ClientTests(unittest.TestCase):
 def test_shared_port_status_no_command_replay(self):
  r=Recorder();h=Hub()
  with patch.dict(SerialHub.registry,{'FAKE':h},clear=True):
   r.attach('FAKE');r._rpc(1);self.assertTrue(r.snapshot()['status']['mounted']);self.assertEqual(len(h.writes),1)
   self.assertEqual(h.writes[0][:4],b'GU\1\6');h.running=False
   with self.assertRaises(IOError):r._rpc(2)
   self.assertEqual(len(h.writes),1);r.close()
 def test_timeout_does_not_retry(self):
  r=Recorder();h=Hub();h.write=lambda p,created:(h.writes.append(p),72)[1];r.hub=h
  with self.assertRaises(TimeoutError):r._rpc(2,timeout=.02)
  self.assertEqual(len(h.writes),1);r.close()
 def test_async_job_and_busy(self):
  r=Recorder();event=threading.Event();r.job(lambda:event.wait(.5))
  with self.assertRaises(ValueError):r.job(lambda:None)
  self.assertTrue(r.snapshot()['busy']);event.set();r.close()
 def test_reconnect_subscribes_new_owner(self):
  r=Recorder();h=Hub()
  with patch.dict(SerialHub.registry,{'FAKE':h},clear=True):
   r.snapshot();self.assertIs(r.hub,h);h.running=False;new=Hub();SerialHub.registry['FAKE']=new;r.snapshot();self.assertIs(r.hub,new);r.close()
 def test_optional_recording_live_without_sd(self):
  r=Recorder();h=Hub()
  with patch.dict(SerialHub.registry,{'FAKE':h},clear=True):
   r.attach('FAKE')
   for b in fragments():h.mail.put((time.monotonic(),4,b))
   until=time.monotonic()+1
   while not r.points and time.monotonic()<until:time.sleep(.01)
   self.assertEqual(r.points[-1]['value'],123);self.assertFalse(r.status.get('mounted',False));r.close()
if __name__=='__main__':unittest.main()
