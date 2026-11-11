"""TC/2 host codec and bounded stop-and-wait client. No GUI dependency."""
import struct,zlib,time,secrets,queue,threading,collections,math
SLOT=128;CHUNK=96;MAX=512
HELLO,READ,WRITE,RESULT,WAVE,STATUS,CONTROL=range(1,8)
def pack(kind,session,seq,payload=b'',timestamp=0):
 if not 1<=kind<=7 or not 1<=session<=0xffffffff or len(payload)>512:raise ValueError('header/length')
 count=max(1,(len(payload)+95)//96);result=[]
 for index in range(count):
  d=payload[index*96:(index+1)*96];b=bytearray(128);struct.pack_into('<2sBBIIIHBBH',b,0,b'TC',2,kind,session,seq,timestamp,len(payload),index,count,len(d));b[28:28+len(d)]=d;struct.pack_into('<I',b,124,zlib.crc32(b[:124]));result.append(bytes(b))
 return result
def unpack(b):
 if len(b)!=128 or b[:3]!=b'TC\x02' or zlib.crc32(b[:124])!=struct.unpack_from('<I',b,124)[0]:raise ValueError('CRC/header')
 _,_,kind,session,seq,stamp,total,index,count,n=struct.unpack_from('<2sBBIIIHBBH',b)
 if not 1<=kind<=7 or not session or total>512 or count!=max(1,(total+95)//96) or index>=count or n!=min(96,total-index*96) or any(b[22:28]) or any(b[28+n:124]):raise ValueError('layout')
 return dict(kind=kind,session=session,seq=seq,timestamp=stamp,total=total,index=index,count=count,data=b[28:28+n])
class Parser:
 def __init__(self):self.buffer=bytearray();self.bad=0
 def feed(self,data):
  self.buffer.extend(data);out=[]
  while len(self.buffer)>=128:
   try:f=unpack(self.buffer[:128])
   except ValueError:self.bad+=1;del self.buffer[0];continue
   out.append(f);del self.buffer[:128]
  return out
class Join:
 def __init__(self):self.key=None;self.next=0;self.data=bytearray();self.at=0
 def feed(self,f,now):
  if self.next and now-self.at>.3:self.key=None;self.next=0
  key=tuple(f[k] for k in ('kind','session','seq','total','count'))
  if f['index']==0:self.key=key;self.next=0;self.data=bytearray();self.at=now
  if self.key!=key or f['index']!=self.next:self.key=None;self.next=0;return None
  self.data.extend(f['data']);self.next+=1
  if self.next==f['count']:self.next=0;self.key=None;return bytes(self.data)
  return None
def wave_decode(d):
 if len(d)<12:raise ValueError('wave length')
 version,ch,n,typ=struct.unpack_from('<BBBB',d);first,period=struct.unpack_from('<II',d,4)
 if version!=1 or not 1<=ch<=16 or not n or typ not in (1,2,3) or not period or len(d)!=12+ch*2+ch*n*4:raise ValueError('wave layout')
 ids=struct.unpack_from('<'+'H'*ch,d,12);values=struct.unpack_from('<'+{1:'I',2:'i',3:'f'}[typ]*(ch*n),d,12+ch*2)
 if typ==3 and not all(math.isfinite(v) for v in values):raise ValueError('nonfinite wave')
 return dict(ids=ids,samples=n,type=typ,first_us=first,period_us=period,values=values)
class Client:
 """One serial owner. Requests bounded to 8, one in flight, 3 retries at 350ms.
 All public methods enqueue; application drains events on its own thread.
 No port scanning, auto connection, or movement command is performed.
 """
 def __init__(self):
  self.waves=queue.Queue(64);self.events=queue.Queue(128);self.commands=queue.Queue(8);self.stop_event=threading.Event();self.thread=None;self.connected=False;self.last_target=0;self.last_radio=0;self.last_peer=0;self.peer_backend=0;self.radio_usable=False;self.pending=None;self.late=0;self.drops=0
 def event(self,*e):
  q=self.waves if e[0]=='wave' else self.events
  try:q.put_nowait(e)
  except queue.Full:
   self.drops+=1
   if e[0]=='wave':
    try:q.get_nowait();q.put_nowait(e)
    except (queue.Empty,queue.Full):pass
 def connect(self,port):
  if self.thread and self.thread.is_alive():raise RuntimeError('already connected')
  self.port=port;self.stop_event.clear();self.thread=threading.Thread(target=self._run,daemon=True);self.thread.start()
 def disconnect(self):self.stop_event.set()
 def request(self,kind,data):
  if not self.connected:return False
  try:self.commands.put_nowait((kind,data));return True
  except queue.Full:self.event('error','请求队列已满');return False
 def _run(self):
  try:
   import serial
   with serial.Serial(self.port,460800,timeout=0,write_timeout=.05) as port:
    port.reset_input_buffer();self.connected=True;self.session=secrets.randbelow(0xfffffffe)+2;self.seq=1;self.parser=Parser();self.assembly=Join();self.pending=None;self.last_target=0;self.last_radio=0;self.last_peer=0;self.peer_backend=0;self.last_wave_seq=None;established=False;heartbeat=0
    while not self.stop_event.is_set():
     now=time.monotonic()
     for f in self.parser.feed(port.read(2048)):
      if f['kind']==STATUS and f['session']==1:
       if len(f['data'])>=4:
        if f['data'][0]==1:self.last_radio=now;self.radio_usable=f['data'][3]!=0
        elif f['data'][0]==2:self.last_peer=now;self.peer_backend=f['data'][1]
       self.event('radio',f['data']);continue
      if f['session']!=self.session:continue
      d=self.assembly.feed(f,now)
      if d is None:continue
      if f['kind']==WAVE:
       if self.last_wave_seq is not None and (f['seq']-self.last_wave_seq)&0xffffffff>=0x80000000:continue
       if f['seq']==self.last_wave_seq:continue
       try:self.event('wave',wave_decode(d));self.last_wave_seq=f['seq']
       except ValueError:self.event('error','波形格式错误')
      elif f['kind']==RESULT:
       if not self.pending or f['seq']!=self.pending['seq'] or len(d)<8 or d[0]!=self.pending['kind']:self.late+=1;continue
       if self.pending['kind'] in (READ,WRITE) and (len(d)!=12 or d[2:4]!=self.pending['frames'][0][28:30]):self.late+=1;continue
       self.last_target=now;heartbeat=now;established=True;self.pending=None;self.event('result',d)
     if self.pending and now-self.pending['at']>=.35:
      if self.pending['retries']>=3:
       self.event('error','应答超时；参数是否应用未知，请重新读取');self.pending=None;established=False;self.session=secrets.randbelow(0xfffffffe)+2;self.last_target=0;self.last_wave_seq=None
       while not self.commands.empty():
        try:self.commands.get_nowait()
        except queue.Empty:break
      else:
       for b in self.pending['frames']:port.write(b)
       self.pending['at']=now;self.pending['retries']+=1
     if self.pending is None:
      command=None
      if not established:command=(HELLO,b'')
      else:
       try:command=self.commands.get_nowait()
       except queue.Empty:
        if now-heartbeat>=.5:command=(READ,struct.pack('<H',1))
      if command:
       self.seq=(self.seq+1)&0xffffffff;kind,data=command;frames=pack(kind,self.session,self.seq,data,int(now*1000)&0xffffffff)
       self.pending=dict(kind=kind,seq=self.seq,frames=frames,at=now,retries=0)
       for b in frames:port.write(b)
     self.stop_event.wait(.005)
  except Exception as e:self.event('error',str(e))
  finally:
   self.connected=False;self.pending=None;self.last_target=0;self.last_radio=0;self.last_peer=0;self.radio_usable=False
   while not self.commands.empty():
    try:self.commands.get_nowait()
    except queue.Empty:break
   self.event('disconnected')
