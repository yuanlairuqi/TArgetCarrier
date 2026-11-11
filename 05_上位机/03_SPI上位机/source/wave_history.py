"""TCWL v1 original-frame files. Does not execute recorded command bytes."""
import struct,zlib,math,csv
from collections import deque
MAGIC=b'TCWL\1\0\0\0'
def u32(p,o=0):return struct.unpack_from('<I',p,o)[0]
def check(p):return len(p)>=4 and zlib.crc32(p[:-4])==u32(p,len(p)-4)
def decode_record(p):
    if len(p)!=40 or p[:3]!=b'CR\1' or not check(p) or p[5] or p[10:12]!=b'\0\0':raise ValueError('采样格式或 CRC 错误')
    kind,typ=p[3:5];channel,unit=struct.unpack_from('<HH',p,6);node,boot,seq,us,bits=struct.unpack_from('<IIIQI',p,12)
    if kind not in (1,2,3) or typ not in (1,2,3) or not node or not boot:raise ValueError('采样元数据无效')
    value=bits if typ==1 else struct.unpack('<i' if typ==2 else '<f',p[32:36])[0]
    if isinstance(value,float) and not math.isfinite(value):raise ValueError('非有限样本')
    return dict(kind=kind,type=typ,channel=channel,unit=unit,node=node,boot=boot,seq=seq,us=us,bits=bits,value=value)
class Decoder:
    def __init__(self):self.join={};self.last={};self.rejected=0;self.gaps=0
    def feed(self,b,ms):
        if len(b)!=64 or b[:3]!=b'WN\1' or not check(b):self.rejected+=1;return None
        if b[3]!=4 or b[32]!=3:return None
        node,boot,epoch,session=struct.unpack_from('<IIII',b,4);key=(node,boot,epoch,session);f=b[40:60]
        version,i,count,n= f[:4];ident=u32(f,4)
        if version!=1 or not ident or not 1<=count<=6 or i>=count or not 1<=n<=12 or (i+1<count and n!=12) or any(f[8+n:]):self.rejected+=1;return None
        state=self.join.get(node)
        if i==0:state=[key,ident,count,0,ms,bytearray()];self.join[node]=state
        if state is None or state[:3]!=[key,ident,count] or state[3]!=i or ms-state[4]>500:
            self.join.pop(node,None);self.rejected+=1;return None
        state[5].extend(f[8:8+n]);state[3]+=1
        if i+1<count:return None
        self.join.pop(node,None)
        try:r=decode_record(bytes(state[5]))
        except ValueError:self.rejected+=1;return None
        if (r['node'],r['boot'])!=(node,boot):self.rejected+=1;return None
        last=self.last.get((node,boot));seq=r['seq']
        if last is not None:
            delta=(seq-last)&0xffffffff
            if not 0<delta<0x80000000:self.rejected+=1;return None
            self.gaps+=delta-1
        self.last[(node,boot)]=seq
        # Bound memory under corrupted source identities.
        if len(self.last)>16:self.last={(node,boot):seq}
        return r

def header(path):
    with open(path,'rb') as f:
        h=f.read(32)
        if len(h)!=32 or h[:4]!=b'TCWL' or h[4] not in (1,2) or any(h[5:8]):raise ValueError('录波文件头损坏')
        if h[4]==2:h+=f.read(16)
        size=48 if h[4]==2 else 32
        if len(h)!=size or not check(h):raise ValueError('录波文件头损坏')
        info=dict(version=h[4],size=size,utc=struct.unpack_from('<Q',h,8)[0],id=u32(h,16),origin=struct.unpack_from('<Q',h,20)[0])
        if h[4]==2:
            info.update(session=struct.unpack_from('<Q',h,28)[0],segment=u32(h,36))
            if not info['session'] or any(h[40:44]):raise ValueError('录制会话标识无效')
        return info

def ordered_paths(paths):
    from pathlib import Path
    if isinstance(paths,(str,Path)):return [Path(paths)]
    items=[(Path(p),header(p)) for p in paths]
    if not items:raise ValueError('请选择原始录波文件')
    if len({str(p.resolve()) for p,_ in items})!=len(items):raise ValueError('重复选择分段文件')
    if len(items)==1:return [items[0][0]]
    versions={h['version'] for _,h in items}
    if len(versions)!=1:raise ValueError('不能混合不同格式的分段')
    if versions=={2}:
        if len({h['session'] for _,h in items})!=1:raise ValueError('选择了不同录制会话')
        if len({(h['utc'],h['origin']) for _,h in items})!=1:raise ValueError('分段时钟基准不一致')
        items.sort(key=lambda x:x[1]['segment'])
        indexes=[h['segment'] for _,h in items]
    else:
        # Legacy files contain no session ID: require explicit selection plus
        # consecutive record IDs and matching UTC/monotonic clock anchors.
        items.sort(key=lambda x:x[1]['id']);indexes=[h['id'] for _,h in items]
        anchors=[h['utc']*1000-h['origin'] for _,h in items]
        if max(anchors)-min(anchors)>5000:raise ValueError('旧格式分段时钟不一致，不能安全拼接')
    if any(b!=a+1 for a,b in zip(indexes,indexes[1:])):raise ValueError('分段缺失或编号重复，请选择连续分段')
    return [p for p,_ in items]

def frames(path,recovery=False):
    """Strict normal reads; recovery returns only the valid prefix after power loss."""
    info=header(path)
    with open(path,'rb') as f:
        f.seek(info['size'])
        while True:
            raw=f.read(80)
            if not raw:return
            if len(raw)!=80 or not check(raw):
                if recovery:return
                raise ValueError('录波尾部截断或 CRC 错误；可用恢复读取保留有效前缀')
            us=struct.unpack_from('<Q',raw)[0]
            b=raw[8:72]
            if b[:3]!=b'WN\1' or not check(b):
                if recovery:return
                raise ValueError('原始无线帧 CRC 错误')
            yield dict(utc_ms=info['utc']+(us-info['origin'])/1000,receive_us=us,body=b,sd_drops=u32(raw,72))

def recording_frames(paths,recovery=False):
    previous_us=None
    for path in ordered_paths(paths):
        for frame in frames(path,recovery):
            if previous_us is not None and frame['receive_us']<previous_us:raise ValueError('分段接收时间倒退，拒绝拼接')
            previous_us=frame['receive_us']
            yield frame

def rows(path,node=None,start=None,end=None,recovery=False):
    decoder=Decoder()
    for f in recording_frames(path,recovery):
        b=f['body'];r=decoder.feed(b,f['receive_us']/1000)
        # Preserve all status and GPIO edge frames as raw rows as well.
        if r is None:
            if b[3]==4 and b[32]==3:continue
            r=dict(node=u32(b,4),boot=u32(b,8),kind='status' if b[3]==3 else 'gpio',seq=u32(b,20),channel='',unit='',type='',us='',bits='',value='')
        if node is not None and r['node']!=node:continue
        at=f['utc_ms']
        if start is not None and at<start or end is not None and at>end:continue
        yield dict(r,utc_ms=at,receive_us=f['receive_us'],sd_drops=f['sd_drops'],raw_hex=b.hex())

def export_csv(path,target,**filters):
    fields=['utc_ms','receive_us','node','boot','kind','channel','unit','type','seq','us','value','bits','sd_drops','raw_hex']
    with open(target,'x',encoding='utf-8-sig',newline='') as f:
        w=csv.DictWriter(f,fieldnames=fields);w.writeheader()
        for r in rows(path,**filters):w.writerow(r)
