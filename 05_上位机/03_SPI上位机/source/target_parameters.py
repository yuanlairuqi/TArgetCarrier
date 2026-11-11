"""Serialized transactions over the project's existing PARAM bridge frames.

READ may retry once. WRITE is issued once, followed by an explicit READ.
There is no transaction ID in this firmware; never overlap register requests.
"""
import math
import struct
from collections import deque
from target_wire import frame

CONFIG={10:('CTRL_MODE','uint'),21:('PMAX','float'),22:('VMAX','float'),23:('TMAX','float')}
EXPECTED={21:12.5,22:45.,23:10.}  # VMAX45 build; manual flash confirmation is a separate gate
# Changing mapping/mode/IDs invalidates the already-flashed transport/decoder.
WRITABLE={0,1,2,3,4,5,6,9,24,25,26,27,28,30,31}

def encode_value(dtype,value):
    if dtype=='uint':
        if type(value) is not int or not 0<=value<=0xffffffff:raise ValueError('uint 参数必须为 0–4294967295 整数')
        return struct.pack('<I',value)
    if dtype!='float' or type(value) not in (int,float) or not math.isfinite(value):raise ValueError('参数必须是有限 float 或 uint')
    try:return struct.pack('<f',value)
    except (OverflowError,struct.error):raise ValueError('float 参数超出 float32 范围')

class TargetParameters:
    def __init__(self,clock):
        self.clock=clock;self.values={};self.current=None;self.todo=deque()
        self.result='尚未读取电机配置';self.config_verified=False;self.write_expected=None
        self.config_result='尚未读取电机配置'
        self.job='';self.quiet_until=0
    @property
    def busy(self):return bool(self.current or self.todo)
    def cancel(self,reason,invalidate=False):
        if self.busy:self.result=reason+'；事务已取消，已发出的写入结果未知，不自动重试'
        self.current=None;self.todo.clear();self.write_expected=None;self.job=''
        self.quiet_until=self.clock()+.6
        if invalidate:self.config_verified=False;self.values={};self.config_result='会话已变化，需重新读取电机配置'
    def start(self,name,values):
        if self.busy or self.clock()<self.quiet_until:raise ValueError('参数事务尚未结束或正在清空旧回复，请稍候')
        self.write_expected=None;self.job=name
        if name=='read_config':
            self.config_verified=False;self.config_result='正在读取电机模式与映射量程'
            items=[dict(rid=rid,dtype=dtype,op=1,raw=bytes(4)) for rid,(_,dtype) in CONFIG.items()]
        else:
            rid=values.get('rid');dtype=values.get('dtype','float')
            if type(rid) is not int or not 0<=rid<=255:raise ValueError('寄存器 RID 必须为 0–255 整数')
            if dtype not in ('uint','float'):raise ValueError('参数类型必须为 uint 或 float')
            op=1;raw=bytes(4)
            if name=='write_parameter':
                if rid not in WRITABLE:raise ValueError('此寄存器禁止在本控制链路写入；模式、ID和映射量程需单独核对')
                required='uint' if rid==9 else 'float'
                if dtype!=required:raise ValueError(f'RID {rid} 必须按 {required} 写入，防止类型不一致损坏参数')
                requested=values.get('value');raw=encode_value(dtype,requested)
                if (rid==0 and requested<=10 or rid==2 and not 80<=requested<200 or
                    rid==3 and not 0<requested<=1 or rid in (1,25,26,27,28) and requested<0 or
                    rid in (4,6,30) and requested<=0 or rid==5 and requested>=0 or
                    rid==24 and not 100<=requested<=10000 or rid==31 and not 1<=requested<=30):
                    raise ValueError(f'RID {rid} 写入值不符合参考手册范围，未发送')
                op=2 if dtype=='uint' else 3
                self.write_expected=(rid,raw)
            items=[dict(rid=rid,dtype=dtype,op=op,raw=raw)]
        self.todo=deque(items);self.result='已排队，等待发送参数请求'
    def next_packet(self,seq):
        now=self.clock()
        if self.current:
            if now-self.current['at']<1.:return None
            if self.current['op']!=1 or self.current['attempts']>=2:
                self.result=f"RID {self.current['rid']} 回复超时；写入不重发，结果未确认"
                self.current=None;self.todo.clear();self.job='';return None
            self.current['attempts']+=1;self.current['at']=now
        elif self.todo:
            self.current=self.todo.popleft();self.current.update(at=now,attempts=1)
        else:return None
        c=self.current
        self.result=f"RID {c['rid']} 已发送{'读取' if c['op']==1 else '写入'}，等待电机回复"
        return frame(4,seq,bytes([c['op'],c['rid']])+c['raw']+bytes(5))
    def receive(self,value):
        c=self.current
        if not c or value['rid']!=c['rid']:return False
        if not value['ok']:
            self.result=f"RID {c['rid']} 被设备拒绝";self.current=None;self.todo.clear();self.job='';return True
        if c['op']!=1:
            # A write ACK is not readback and never counts as applied.
            self.todo.appendleft(dict(rid=c['rid'],dtype=c['dtype'],op=1,raw=bytes(4)))
            self.result=f"RID {c['rid']} 写入回复已到，等待独立读取核对";self.current=None;return True
        raw=bytes.fromhex(value['raw']);v=struct.unpack('<I' if c['dtype']=='uint' else '<f',raw)[0]
        if c['dtype']=='float' and not math.isfinite(v):
            self.result='参数回复包含非有限值，拒绝使用';self.current=None;self.todo.clear();self.job='';return True
        self.values[c['rid']]=dict(name=CONFIG.get(c['rid'],(str(c['rid']),))[0],value=v,raw=raw.hex(),dtype=c['dtype'])
        if self.write_expected:
            rid,wanted=self.write_expected
            self.result=f'RID {rid} 回读'+('一致（未执行掉电保存，机械效果未验证）' if raw==wanted else '不一致；未确认生效')
            self.write_expected=None
        else:self.result=f"RID {c['rid']} 读取成功：{v}"
        self.current=None
        if not self.todo:
            if self.job=='read_config':
                mismatches=[f"{CONFIG[r][0]}={self.values[r]['value']}（源码 {expected}）" for r,expected in EXPECTED.items()
                            if not math.isclose(self.values[r]['value'],expected,rel_tol=1e-5,abs_tol=1e-5)]
                self.config_verified=not mismatches
                self.result=('映射量程与当前 STM32 源码一致；控制模式原始值='+str(self.values[10]['value'])+
                             '，仍需核对实际模式与已烧录版本') if not mismatches else '映射量程不一致，禁止使能：'+'；'.join(mismatches)
                self.config_result=self.result
            self.job=''
        return True
    def snapshot(self):
        return dict(values={str(k):dict(v) for k,v in self.values.items()},busy=self.busy,result=self.result,
                    config_verified=self.config_verified,config_result=self.config_result,expected={str(k):v for k,v in EXPECTED.items()})
