"""WN/1 kind 5: target 17-byte UART payload; role/node=2 only."""
import asyncio,struct,time,zlib,secrets
from usb_transport import UsbClient,wn_frame
from target_wire import STATUS,decode
from target_ble import BleTargetAdapter
class TargetUsbClient(UsbClient):
    def __init__(self,device,**kw):
        kw.pop('winrt',None);super().__init__(device,node=2,**kw)
        self.outer_seq=0;self.cached_health=None;self.on_link_loss=None
    async def read_gatt_char(self,uuid):
        return self.cached_health or b''
    async def write_gatt_char(self,tx,data,response=True):
        if not self.challenge:
            await self.send(wn_frame(1,2));return
        # Until STOP handshake: drop gains/read/write/motion; periodic disabled
        # controls from the existing adapter will acquire the session safely.
        stop=len(data)==17 and data[2]==1 and (not data[4]&1 or data[4]&12)
        if not self.control_ready and not stop:return
        self.outer_seq=(self.outer_seq+1)&0xffffffff
        await self.send(wn_frame(5,2,*self.challenge,self.session,self.outer_seq,bytes(data)),self.generation)
    def receive(self,kind,b):
        if kind==5:return super().receive(kind,b)
        if (kind!=2 or len(b)!=64 or b[:4]!=b'WN\x01\x03' or
            struct.unpack_from('<I',b,4)[0]!=2 or zlib.crc32(b[:60])!=struct.unpack_from('<I',b,60)[0]):return
        boot,epoch,session=struct.unpack_from('<III',b,8);challenge=(boot,epoch)
        if not boot or not epoch or challenge in self.retired:return
        if self.challenge and challenge!=self.challenge:
            if boot==self.challenge[0] and (epoch-self.challenge[1])&0xffffffff>=0x80000000:return
            self.retired.append(self.challenge);self.invalidate('靶机新会话；不恢复旧运动')
        self.last_seen=self.last_usb=time.monotonic()
        if self.challenge!=challenge:
            self.challenge=challenge;self.session=secrets.randbelow(0xffffffff)+1;self.outer_seq=0;return
        if b[24]!=0 or b[25]!=2 or session!=self.session:
            if self.control_ready:self.invalidate('靶机控制会话失效')
            return
        n=b[27]
        if n not in (17,20):return
        raw=b[40:40+n]
        try:name,payload=decode(raw)
        except ValueError:return
        self.control_ready=True;self.loss_reported=False;self.last_radio=time.monotonic()
        if name=='health':self.cached_health=raw
        self.deliver(STATUS,raw)
class UsbTargetAdapter(BleTargetAdapter):
    def __init__(self,**kw):
        super().__init__(**kw);self.usb_port=None
    def connect_usb(self,key):
        with self.lock:
            if self.endpoint:
                if self.endpoint==key:return True
                raise ValueError('请先断开靶机当前连接')
            self.usb_port=key;self.devices[key]=key;self.factory=TargetUsbClient
            self.notice='USB → GEEK → 节点2 → 靶机 STM32/CAN'
        return super().connect(key)
    def scan(self):
        if self.usb_port:raise ValueError('GEEK模式已指定节点2；无需搜索靶机BLE')
        return super().scan()
    def snapshot(self):
        s=super().snapshot()
        if self.usb_port:
            s['transport']='USB → GEEK → ESP-NOW · 节点2'
            s['phase']=s['phase'].replace('BLE','无线').replace('蓝牙','无线')
            c=self.client
            s['wireless']=dict(node=2,usb_online=bool(c and c.is_connected),
                slave_accepted=bool(c and c.transport_ready()),diagnostic=getattr(c,'diagnostic',None))
        return s
