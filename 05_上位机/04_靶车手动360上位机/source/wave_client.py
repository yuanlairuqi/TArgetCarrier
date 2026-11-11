"""Optional SD service sharing SerialHub. File jobs never run on control/UI threads."""
import struct,zlib,time,secrets,queue,threading
from pathlib import Path
from collections import deque
from serial_hub import SerialHub
from wave_history import Decoder,rows,export_csv
ERRORS={1:'SD 卡未挂载：检查插卡与 FAT32 格式',2:'SD 读写失败，录制已停止',3:'正在录制：请先停止后管理历史',4:'无效录波请求',5:'记录不存在',6:'卡剩余空间不足，录制停止',7:'记录编号空间已满',8:'请求编号冲突'}
def packet(body):
    p=b'GU\1\6'+body;return p+struct.pack('<I',zlib.crc32(p))
class Recorder:
    def __init__(self):
        self.lock=threading.RLock();self.hub=None;self.mail=None;self.replies=queue.Queue(32);self.decoder=Decoder();self.points=deque(maxlen=2000)
        self.history=[];self.replay=[];self.downloads={};self.busy=False;self.message='连接 GEEK USB 后可查看；SD 可选';self.status={};self.status_at=0;self.thread=None;self.closing=False;self.job_generation=0
    def attach(self,port):
        with SerialHub.registry_lock:hub=SerialHub.registry.get(port)
        if not hub or not hub.running:raise ValueError('请先在设备控制页连接这个 GEEK USB 串口')
        with self.lock:
            if hub is self.hub:return
            if self.busy:raise ValueError('录波任务进行中')
            if self.hub and self.mail:self.hub.capture_unsubscribe(self.mail)
            self.hub=hub;self.mail=hub.capture_subscribe(512);self.decoder=Decoder();self.points.clear();self.status={};self.status_at=0;self.job_generation+=1
            self.thread=threading.Thread(target=self._reader,args=(hub,self.mail),daemon=True);self.thread.start()
    def _reader(self,hub,mail):
        while not self.closing and hub.running and self.hub is hub and self.mail is mail:
            try:at,kind,b=mail.get(timeout=.1)
            except queue.Empty:continue
            if kind==7 and b[:3]==b'RS\1' and b[10]<=44:
                with self.lock:
                    if b[3] in (1,2,3,8) and b[10]>=24:
                        self.status=dict(mounted=bool(b[9]&1),recording=bool(b[9]&2),id=struct.unpack_from('<I',b,16)[0],drops=struct.unpack_from('<I',b,20)[0],size=struct.unpack_from('<I',b,24)[0],fault=b[36]);self.status_at=time.monotonic()
                if struct.unpack_from('<I',b,4)[0]:
                    try:self.replies.put_nowait(b)
                    except queue.Full:pass
            elif kind==4:
                with self.lock:
                    row=self.decoder.feed(b,at*1000)
                    if row:row['utc_ms']=int(time.time()*1000);self.points.append(row)
    def _rpc(self,op,ident=0,offset=0,data=b'',timeout=3):
        with self.lock:hub=self.hub;generation=self.job_generation
        if not hub or not hub.running:raise IOError('GEEK USB 已断开；录制状态未知，请重连查询')
        nonce=secrets.randbelow(0xffffffff)+1;q=bytearray(64);q[:4]=b'RC\1'+bytes([op]);struct.pack_into('<III',q,4,nonce,ident,offset);q[16]=len(data);q[20:20+len(data)]=data
        if len(data)>44:raise ValueError('备注最多 44 字节 UTF-8')
        if hub.write(packet(bytes(q)),time.monotonic())!=72:raise IOError('请求未发送；请重试查询状态')
        deadline=time.monotonic()+timeout
        while time.monotonic()<deadline:
            if self.closing or generation!=self.job_generation or not hub.running:raise IOError('USB 中断；操作结果未知，请重连查询')
            try:b=self.replies.get(timeout=.05)
            except queue.Empty:continue
            if struct.unpack_from('<I',b,4)[0]!=nonce or b[3]!=op:continue
            if b[8]:raise IOError(ERRORS.get(b[8],f'录波错误 {b[8]}'))
            return b
        raise TimeoutError('GEEK 应答超时；结果未知，请查询状态，不自动重放请求')
    def job(self,fn):
        with self.lock:
            if self.busy:raise ValueError('已有录波任务进行中')
            self.busy=True;self.message='正在处理…'
        def run():
            try:
                result=fn()
                with self.lock:self.message=result or '完成'
            except Exception as e:
                with self.lock:self.message=str(e)
            finally:
                with self.lock:self.busy=False
        threading.Thread(target=run,name='wave-file-job',daemon=True).start();return True
    def action(self,op):
        if op not in (1,2,3,8):raise ValueError('未知录波操作')
        return self.job(lambda:(self._rpc(op,data=struct.pack('<Q',int(time.time()*1000)) if op==2 else b''),'已确认操作；请查看 SD 状态')[1])
    def list_history(self):
        def run():
            out=[];cursor=0
            for _ in range(1000):
                b=self._rpc(4,offset=cursor)
                if not b[10]:break
                ident=struct.unpack_from('<I',b,16)[0]
                if ident<=cursor:raise ValueError('历史游标没有前进')
                cursor=ident
                try:note=self._rpc(9,ident)[20:64].rstrip(b'\0').decode('utf8',errors='replace')
                except IOError:note=''
                out.append(dict(id=ident,size=struct.unpack_from('<I',b,20)[0],utc_ms=struct.unpack_from('<Q',b,24)[0],note=note))
            with self.lock:self.history=out
            return f'已读取 {len(out)} 条历史（每次最多 1000 条）'
        return self.job(run)
    def note(self,ident,text):return self.job(lambda:(self._rpc(6,ident,data=str(text).encode('utf8')),'备注已保存')[1])
    def delete(self,ident):
        def run():
            self._rpc(7,ident)
            with self.lock:self.history=[h for h in self.history if h['id']!=ident];self.downloads.pop(ident,None)
            return '已删除 SD 记录'
        return self.job(run)
    def download(self,ident,target):
        def run():
            target_path=Path(target);part=target_path.with_name(target_path.name+'.partial')
            offset=0
            # Exclusive writes preserve existing files, including failed downloads.
            if target_path.exists():raise FileExistsError('目标已存在，请选择新文件名')
            with part.open('xb') as f:
                while True:
                    b=self._rpc(5,ident,offset);n=b[10]
                    if struct.unpack_from('<II',b,12)!=(offset,ident):raise IOError('下载应答偏移错误')
                    if not n:break
                    f.write(b[20:20+n]);offset+=n
                    with self.lock:self.message=f'正在下载 {offset} 字节；控制与停止仍可用'
                    if offset>64*1024*1024:raise ValueError('记录大小越界')
            # Check complete file CRCs before committing. Raw .partial survives errors.
            from wave_history import frames
            for _ in frames(part):pass
            # rename cannot replace an existing target on Windows.
            part.rename(target_path)
            with self.lock:self.downloads[ident]=str(target_path)
            return f'原始文件已保存：{target_path.name}'
        return self.job(run)
    def replay_file(self,path,node=None,start=None,end=None,recovery=False):
        def run():
            # Bounded view; CSV export streams the complete selected range.
            view=deque(maxlen=2000)
            for row in rows(path,node,start,end,recovery):view.append(row)
            with self.lock:self.replay=list(view)
            return f'回放显示所选范围最后 {len(view)} 条；原始文件完整保留'
        return self.job(run)
    def export_file(self,path,target,node=None,start=None,end=None,recovery=False):
        return self.job(lambda:(export_csv(path,target,node=node,start=start,end=end,recovery=recovery),'CSV 已导出')[1])
    def snapshot(self):
        with SerialHub.registry_lock:ports=[p for p,h in SerialHub.registry.items() if h.running]
        if (not self.hub or not self.hub.running) and len(ports)==1 and not self.busy:self.attach(ports[0])
        with self.lock:
            online=bool(self.hub and self.hub.running);fresh=online and time.monotonic()-self.status_at<3
            return dict(ports=ports,port=self.hub.port if self.hub else '',online=online,fresh=fresh,status=dict(self.status),busy=self.busy,message=self.message,points=list(self.points)[-300:],history=list(self.history),replay=list(self.replay)[-2000:],rejected=self.decoder.rejected,gaps=self.decoder.gaps,observer_drops=self.hub.capture_drops if self.hub else 0)
    def close(self):
        self.closing=True
        if self.hub and self.mail:self.hub.capture_unsubscribe(self.mail)
