"""Wire-compatible port of APK Protocol, CommandChannel and PulseHistory."""
import struct
import zlib
from collections import deque
from dataclasses import dataclass

SERVICE = '6e400001-b5a3-f393-e0a9-e50e24dcca9e'
COMMAND = SERVICE.replace('0001-', '0002-')
STATUS = SERVICE.replace('0001-', '0003-')
PULSE = SERVICE.replace('0001-', '0004-')
MOTIONS = ['停止', '正转', '反转', '保留', '保留', '本地动作']
ERRORS = ['正常', '指令错误', '指令超时，已停止', '连接断开，已停止', '电压采样异常',
          '串口未配置', 'STM32 离线', 'STM32 拒绝或故障', '串口发送失败', 'STM32 确认超时']

def command(motion, speed, seq, mode=0, forward=0, reverse=0):
    if (motion not in (0, 1, 2) or not 0 <= speed <= 100 or
        (motion == 0) != (speed == 0) or mode not in (0, 1) or
        mode == 1 and not (100 <= forward <= 3600000 and 100 <= reverse <= 3600000)):
        raise ValueError('无效控制参数')
    if motion == 0:
        mode = forward = reverse = 0
    return struct.pack('<BBBBIII', 3, motion, speed, mode, seq & 0xffffffff, forward, reverse)

@dataclass(frozen=True)
class Status:
    motion: int
    flags: int
    battery: int
    requested: int
    speed: int
    error: int
    ack: int
    stm_ack: int
    uptime: int
    mode: int
    phase: int
    @property
    def uart(self): return bool(self.flags & 128)
    @property
    def online(self): return bool(self.flags & 8)
    @property
    def demo(self): return bool(self.flags & 2)
    @property
    def can_move(self):
        return bool(self.flags & 1) and self.motion != 5 and (self.demo if not self.uart else
            self.online and bool(self.flags & 16) and not bool(self.flags & 64) and self.error == 0)
    @property
    def voltage(self): return self.battery / 1000 if self.flags & 4 and self.battery != 65535 else None

def parse_status(data):
    if len(data) != 20 or data[0] != 3:
        raise ValueError('状态帧长度或版本不匹配')
    s = Status(*struct.unpack('<BBHBBBIIHBB', data[1:]))
    if (s.motion not in (0, 1, 2, 5) or s.requested > 2 or s.speed > 100 or s.error > 9 or
        s.mode > 1 or s.phase > 2 or (s.flags & 2 and s.flags & 32) or
        (not s.online and s.flags & (16 | 32 | 64))):
        raise ValueError('状态字段无效')
    return s

@dataclass
class Packet:
    seq: int
    motion: int
    created: float
    data: bytes
    sent: float = 0

class Channel:
    def __init__(self):
        self.seq = 0
        self.pending = self.writing = None
        self.unconfirmed = deque()
        self.barrier = self.synced = self.timed_out = False
        self.stop_seq = 0
    def packet(self, motion, speed, now, mode=0, forward=0, reverse=0):
        self.seq = (self.seq + 1) & 0xffffffff
        return Packet(self.seq, motion, now, command(motion, speed, self.seq, mode, forward, reverse))
    def move(self, motion, speed, now, mode=0, forward=0, reverse=0):
        if not self.synced or self.barrier or motion == 0: return False
        self.pending = self.packet(motion, speed, now, mode, forward, reverse)
        return True
    def stop(self, now, force=False):
        self.unconfirmed = deque(p for p in self.unconfirmed if p.motion == 0)
        if self.barrier and not self.timed_out and not force: return
        # A new STOP attempt has its own ACK clock. Expired STOPs and late ACKs
        # must neither immediately time out nor unlock this fresh barrier.
        self.unconfirmed.clear()
        self.pending = self.packet(0, 0, now)
        self.stop_seq = self.pending.seq
        self.barrier, self.timed_out = True, False
    def take(self, now):
        if self.writing or not self.pending: return None
        if self.pending.motion and (now - self.pending.created > .3 or len(self.unconfirmed) >= 4):
            self.pending = None
            return None
        p, self.pending = self.pending, None
        p.sent = now
        self.writing = p
        self.unconfirmed.append(p)
        return p
    def expired(self, now):
        if self.timed_out or not self.unconfirmed or now - self.unconfirmed[0].sent < 1: return None
        self.timed_out = True
        return self.unconfirmed[0].motion
    def status(self, s):
        ack = s.stm_ack if s.uart else s.ack
        matched = next((p for p in self.unconfirmed if p.seq == ack), None)
        if matched is None: return
        hardware = s.online and not s.demo and bool(s.flags & 32) and not s.flags & 64 and s.error == 0 and s.ack == s.stm_ack
        if s.uart and matched.motion and (not hardware or s.mode != matched.data[3] or
            s.speed != matched.data[2] or (matched.data[3] == 0 and
            (s.motion != matched.motion or s.requested != matched.motion or s.phase != matched.motion))): return
        valid_stop = (ack == self.stop_seq == s.ack and s.motion == 0 and s.requested == 0 and
                      s.mode == 0 and s.phase == 0 and (not s.uart or hardware))
        if self.barrier and ack == self.stop_seq and not valid_stop: return
        while self.unconfirmed:
            if self.unconfirmed.popleft().seq == ack: break
        if self.barrier and valid_stop:
            self.barrier, self.synced, self.timed_out = False, True, False

def link_description(s):
    """BLE reception, UART configuration, peer health and output are distinct."""
    if not s.uart:
        return 'BLE 正常 · S3 串口未启用（演示）', '需要启用配套 S3 UART 固件，电脑不能代替串口通信'
    if not s.online:
        return 'BLE 正常 · STM32 无有效串口回包', '检查 STM32 供电/控制固件；S3 G4→PB11、G5←PB10、共地，115200 8N1'
    if s.flags & 64 or s.error in (7, 8, 9):
        return 'BLE 正常 · STM32 已在线，控制链路异常', ERRORS[s.error] if s.error else 'STM32 报告故障'
    if s.motion == 5:
        return 'BLE 正常 · STM32 本地动作占用', '等待本地动作结束，恢复后重新确认停止'
    if not s.flags & 16:
        return 'BLE 正常 · STM32 未就绪', '检查 STM32 连续控制配置；当前保持停止'
    return 'BLE 正常 · STM32 串口在线', '停止确认通过后可以手动开始；链路恢复不会自动恢复运动'

class PulseAssembler:
    def __init__(self):
        self.header = None
        self.latest = None
        self.errors = 0
        self.state = '等待硬件采集数据'
    def receive(self, b):
        try: return self._receive(bytes(b))
        except (ValueError, struct.error) as e:
            self.errors += 1
            self.header = None
            self.state = str(e) + '；等待下一段完整采样'
            return None
    def _receive(self, b):
        if len(b) != 20 or b[0] != 0x57: raise ValueError('波形帧格式错误')
        kind, ident = b[1], struct.unpack_from('<H', b, 2)[0]
        if kind == 1:
            flags, initial, hz, duration, count, pul, direction, version, reserved = struct.unpack_from('<BBIIHBBBB', b, 4)
            if (version != 1 or reserved or count > 512 or flags & ~7 or initial & ~3 or
                flags & 1 and (not hz or not duration or duration > hz or pul > 127 or direction > 127 or pul == direction)):
                raise ValueError('采样头无效')
            if self.header is not None: self.errors += 1
            self.header, self.ident = b, ident
            self.edges, self.sequence = [], 0
            self.crc = zlib.crc32(b)
            self.state = '正在接收采样 #' + str(ident)
            return None
        if self.header is None or ident != self.ident: raise ValueError('采样序号不连续')
        flags, initial, hz, duration, count, pul, direction, _, _ = struct.unpack_from('<BBIIHBBBB', self.header, 4)
        if kind == 2:
            if b[4] != self.sequence or len(self.edges) >= count: raise ValueError('波形缺包或重复')
            for k in range(min(3, count - len(self.edges))):
                offset = 5 + k * 5
                tick, tag = struct.unpack_from('<IB', b, offset)
                if tick >= duration or tag > 3 or self.edges and tick < self.edges[-1][0]:
                    raise ValueError('边沿时间或通道无效')
                self.edges.append((tick, tag))
                self.crc = zlib.crc32(b[offset:offset+5], self.crc)
            self.sequence += 1
            return None
        chunks, total, crc = struct.unpack_from('<HHI', b, 4)
        if kind != 3 or len(self.edges) != count or chunks != self.sequence or total != count or crc != self.crc:
            raise ValueError('采样不完整或 CRC 校验失败')
        levels = initial
        for tick, tag in self.edges:
            ch, value = tag & 1, tag >> 1
            if (levels >> ch) & 1 == value: raise ValueError('边沿电平不连续')
            levels = (levels & ~(1 << ch)) | (value << ch)
        self.latest = dict(id=ident, flags=flags, initial=initial, hz=hz, duration=duration,
                           pul=pul, direction=direction, edges=tuple(self.edges))
        self.header = None
        self.state = '完整硬件采样 #' + str(ident) if flags == 1 else '采集未启用或存在丢失，本段不绘图'
        return self.latest
