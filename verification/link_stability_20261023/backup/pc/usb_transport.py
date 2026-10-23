"""USB GU/1 -> authenticated GEEK WN/1 transport; BLE business logic is reused.
No motion retransmission. Fresh transactions every 200 ms are existing leases,
not retries. Recovery clears the challenge and requires a fresh STOP handshake.
"""
import asyncio
import struct
import time
import secrets
import zlib
import json
import sys
from pathlib import Path
from types import SimpleNamespace
from protocol import COMMAND, STATUS, PULSE

def wn_frame(kind,node,boot=0,epoch=0,session=0,seq=0,command=b''):
    body=bytearray(64)
    struct.pack_into('<2sBBIIIII',body,0,b'WN',1,kind,node,boot,epoch,session,seq)
    body[24:24+len(command)]=command
    struct.pack_into('<I',body,60,zlib.crc32(body[:60]))
    return bytes(body)

def usb_frame(kind,body):
    if len(body)!=64:raise ValueError('USB body must be 64 bytes')
    packet=b'GU'+bytes([1,kind])+body
    return packet+struct.pack('<I',zlib.crc32(packet))

class Parser:
    def __init__(self):self.buffer=bytearray();self.errors=0
    def feed(self,data):
        self.buffer.extend(data);out=[]
        while len(self.buffer)>=72:
            p=self.buffer[:72]
            if p[:3]!=b'GU\x01' or zlib.crc32(p[:68])!=struct.unpack_from('<I',p,68)[0]:
                del self.buffer[0];self.errors+=1;continue
            del self.buffer[:72];out.append((p[3],bytes(p[4:68])))
        return out

class Services:
    def get_service(self,_):return self
    def get_characteristic(self,uuid):return SimpleNamespace(uuid=uuid,properties=['write','notify'])

class UsbClient:
    def __init__(self,device,timeout=12,serial_factory=None,node=None):
        base=Path(sys.executable).parent if getattr(sys,'frozen',False) else Path(__file__).resolve().parent
        config=base/'usb.local.json'
        if not config.exists():config=base.parent/'usb.local.json'
        if node is None:node=json.loads(config.read_text(encoding='utf8')).get('node',1) if config.exists() else 1
        if type(node) is not int or not 1<=node<=65535:raise ValueError('USB node ID must be 1..65535')
        self.port=device[4:];self.node=node;self.serial_factory=serial_factory
        self.callbacks={};self.services=Services();self.is_connected=False
        self.challenge=None;self.session=secrets.randbelow(0xffffffff)+1
        self.parser=Parser();self.wave=None;self.last_radio=0;self.last_query=0
        self.info=None;self.diagnostic=None
    async def connect(self):
        if self.serial_factory is None:
            import serial
            self.serial_factory=serial.Serial
        self.serial=await asyncio.to_thread(self.serial_factory,self.port,115200,timeout=0,write_timeout=.1)
        self.tx_lock=asyncio.Lock();self.is_connected=True;self.task=asyncio.create_task(self.reader())
    async def disconnect(self):
        self.is_connected=False
        if hasattr(self,'task'):
            self.task.cancel();await asyncio.gather(self.task,return_exceptions=True)
        if hasattr(self,'serial'):await asyncio.to_thread(self.serial.close)
    async def start_notify(self,uuid,callback):self.callbacks[uuid]=callback
    async def send(self,body):
        packet=usb_frame(1,body)
        async with self.tx_lock:
            n=await asyncio.to_thread(self.serial.write,packet)
        if n!=len(packet):raise IOError('USB 写入不完整；不重试运动命令')
    async def write_gatt_char(self,tx,data,response=True):
        del tx,response
        if not self.challenge:
            # STOP stays pending in the existing controller until a real slave status arrives.
            await self.send(wn_frame(1,self.node));return
        seq=struct.unpack_from('<I',data,4)[0]
        await self.send(wn_frame(2,self.node,*self.challenge,self.session,seq,bytes(data)))
    def deliver(self,uuid,data):
        if uuid in self.callbacks:self.callbacks[uuid](uuid,data)
    def receive(self,kind,b):
        if kind==5:
            self.info=dict(provisioned=bool(b[0]),channel=b[1],node=struct.unpack_from('<I',b,4)[0],mac=b[12:18].hex(':'))
            self.info.update(zip(('submitted','link_ok','link_fail','rejected','expired','control_drop'),struct.unpack_from('<6I',b,20)))
            if not b[0]:self.diagnostic='GEEK 未配置单播 MAC/密钥；无线未启用'
            return
        if (kind not in (2,4) or b[:3]!=b'WN\x01' or b[3]!=(3 if kind==2 else 4) or
            struct.unpack_from('<I',b,4)[0]!=self.node or zlib.crc32(b[:60])!=struct.unpack_from('<I',b,60)[0]):return
        boot,epoch,session=struct.unpack_from('<III',b,8)
        if kind==2:
            challenge=(boot,epoch)
            if self.challenge!=challenge:
                self.challenge=challenge;self.wave=None
                # A new challenge must never acknowledge any old PC transaction.
                self.session=secrets.randbelow(0xffffffff)+1
                s=bytearray(b[40:60]);s[2]&=~32;s[7]=3;s[8:16]=b'\0'*8
                self.deliver(STATUS,s);return
            result,owner,request_kind=b[24:27]
            self.diagnostic=f'从机接收结果={result} 控制权={owner}；STM ACK 单独校验'
            if result not in (0,5) or owner!=2 or session!=self.session:
                self.wave=None
                s=bytearray(b[40:60]);s[2]&=~32;s[7]=3;s[8:16]=b'\0'*8
                self.deliver(STATUS,s);return
            self.last_radio=time.monotonic();self.deliver(STATUS,b[40:60])
        else:
            if self.challenge!=(boot,epoch) or session!=self.session:return
            start,index,total=struct.unpack_from('<IHH',b,24)
            raw=b[40:60];capture_id=struct.unpack_from('<H',raw,2)[0]
            identity=(boot,epoch,session,start,capture_id,total)
            now=time.monotonic()
            if index==0:
                if raw[1]!=1 or not 2<=total<=173 or b[32]!=1:return
                self.wave=[identity,0,now]
            if not self.wave or self.wave[0]!=identity or index!=self.wave[1] or now-self.wave[2]>5:
                self.wave=None;self.deliver(PULSE,b'');return
            self.wave[1]+=1;self.capture_meta=dict(node=self.node,boot=boot,epoch=epoch,
                source='slave MCPWM GPIO capture',start_ms=start,received_at=time.time())
            self.deliver(PULSE,raw)
            if index+1==total:self.wave=None
    async def reader(self):
        try:
            while self.is_connected:
                data=self.serial.read(576)
                for kind,body in self.parser.feed(data):self.receive(kind,body)
                now=time.monotonic()
                if self.wave and now-self.wave[2]>5:self.wave=None;self.deliver(PULSE,b'')
                if (self.challenge is None or now-self.last_radio>1) and now-self.last_query>.2:
                    self.last_query=now;await self.send(wn_frame(1,self.node))
                await asyncio.sleep(.005)
        except asyncio.CancelledError:raise
        except Exception as e:
            self.diagnostic=str(e);self.is_connected=False

from controller import Controller
class CombinedController(Controller):
    def __init__(self,events):
        super().__init__(events,client_factory=self.client_for)
    def idle_maintenance(self, now):
        # A fresh STOP keeps the slave's 800 ms command lease alive while idle.
        # Use the same Channel/sequence and require a fresh STM confirmation.
        # Never replace a pending transaction or acknowledge STOP locally.
        if (isinstance(self.client, UsbClient) and self.available() and self.held is None
            and self.channel.pending is None and self.channel.writing is None
            and not self.channel.unconfirmed and now-self.last_stop >= .4):
            self.stop(force=True)
    @staticmethod
    def client_for(device,**kw):
        if isinstance(device,str) and device.startswith('usb:'):return UsbClient(device,**kw)
        from bleak import BleakClient
        return BleakClient(device,**kw)
    def _request(self, name, args):
        if name == 'scan_usb':
            if self.session or self.operation and not self.operation.done():
                self.emit('busy', bool(self.session or self.operation)); return
            self.operation = asyncio.create_task(self.scan(include_ble=False))
        else:
            super()._request(name, args)

    async def scan(self, include_ble=True):
        self.emit('busy', True)
        if include_ble:
            await super().scan()
        else:
            self.devices = {}
        descriptions = {}
        try:
            from serial.tools import list_ports
            for p in list_ports.comports():
                key = 'usb:'+p.device
                self.devices[key] = key
                descriptions[key] = p.description or p.device
        except Exception as e:self.report('USB 搜索失败：'+str(e)+'；BLE 功能保留')
        rows=[]
        for key,device in self.devices.items():
            rows.append((key,'USB '+key[4:]+' · '+descriptions.get(key,'串口设备，需核对 GEEK') if key.startswith('usb:') else getattr(device,'name','ESP32-Robot'),None))
        self.emit('devices',rows);self.emit('connection','请选择 BLE 设备或核对 GEEK 的 USB COM 口' if rows else '未找到设备，请检查 USB 连接或搜索 BLE');self.emit('busy',False)
    def emit_health(self,now=None):
        super().emit_health(now)
        if isinstance(self.client,UsbClient):
            info=self.client.info or {}
            self.emit('transport',dict(name='USB → GEEK → ESP-NOW',node=self.client.node,
                parser_errors=self.client.parser.errors, radio_age_ms=round((time.monotonic()-self.client.last_radio)*1000) if self.client.last_radio else None, diagnostic=f'{self.client.diagnostic or "等待从机"} · 无线提交 {info.get("submitted",0)} / MAC成功 {info.get("link_ok",0)} / 失败 {info.get("link_fail",0)}；MAC成功不代表执行',info=info))
    def receive_pulse(self,sender,data):
        capture=self.pulses.receive(data)
        if capture:
            if isinstance(self.client,UsbClient):capture.update(getattr(self.client,'capture_meta',{}))
            self.emit('pulse',capture)
        now=time.monotonic()
        if capture or now-self.last_pulse_event>=.2:
            self.last_pulse_event=now
            self.emit('pulse_state',self.pulses.state+'；异常 '+str(self.pulses.errors))
