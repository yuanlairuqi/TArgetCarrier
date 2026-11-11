"""Target's existing UART protocol over its dedicated BLE service (17 B <= ATT 20)."""
from manual_speed_limits import WIRE_RATE_MAX_DPS
import math
import struct

SERVICE = '6e410001-b5a3-f393-e0a9-e50e24dcca9e'
COMMAND = SERVICE.replace('0001-', '0002-')
STATUS = SERVICE.replace('0001-', '0003-')


def crc16(data):
    crc = 0xffff
    for b in data:
        crc ^= b
        for _ in range(8): crc = (crc >> 1) ^ (0xa001 if crc & 1 else 0)
    return crc


def frame(kind, seq, payload):
    data = bytearray([0xaa, 13, kind, seq & 255]) + bytearray(payload)
    if len(data) != 15: raise ValueError('Target payload length')
    return bytes(data) + bytes([crc16(data[1:15]) & 255, 0x55])


VELOCITY_FLAG = 0x40
CAPABILITY_RID = 254
CAPABILITY_VALUE = 0x00060101

def orientation(degrees):
    if type(degrees) not in (int,float) or not math.isfinite(degrees):raise ValueError('角度必须为有限数值')
    return (degrees+180.)%360.-180.

def quantized_gains(kp,kd):
    if not all(type(v) in (int,float) and math.isfinite(v) for v in (kp,kd)) or not 0<=kp<=200 or not 0<=kd<=3:
        raise ValueError('kp/kd 范围无效')
    qkp,qkd=int(kp/.1+.5),int(kd/.01+.5)
    if kp>0 and (qkp==0 or int(qkp*.1*4095/500)==0) or kd>0 and qkd==0:
        raise ValueError('两级编码下非零 kp 最小 0.2，非零 kd 最小 0.01；零增益请明确输入 0')
    return qkp*.1,qkd*.01

def control(seq, flags, yaw, pitch, yaw_rate=0, pitch_rate=0):
    values = [(yaw, -180, 180, .01), (pitch, -30, 60, .01),
              (yaw_rate, -WIRE_RATE_MAX_DPS if flags & VELOCITY_FLAG else -180, WIRE_RATE_MAX_DPS if flags & VELOCITY_FLAG else 180, .1), (pitch_rate, -180, 180, .1)]
    if not all(type(v) in (int, float) and math.isfinite(v) and lo <= v <= hi for v, lo, hi, scale in values):
        raise ValueError('靶车目标超出角度/速度范围')
    # protocol.h rounds halves away from zero.
    q = [int(v/scale + (.5 if v >= 0 else -.5)) for v, lo, hi, scale in values]
    return frame(1, seq, struct.pack('<HBhhhh', flags, 0, *q))


def gains(seq, kp, kd):
    kp,kd=quantized_gains(kp,kd)
    return frame(3, seq, struct.pack('<HH7x', int(kp/.1+.5), int(kd/.01+.5)))


def decode(data):
    data = bytes(data)
    if len(data) == 20 and data[:3] == b'TB\x01':
        flags, boot, epoch, age, motor_age, seq, drops, errors = struct.unpack('<BIIHHBBH', data[3:])
        return 'health', dict(flags=flags, boot=boot, epoch=epoch, age=age, motor_age=motor_age,
                              ctrl_seq=seq, rejected=drops, uart_errors=errors)
    if len(data) != 17 or crc16(data[1:16]) & 255 != data[16] and data[0] == 0x5a:
        raise ValueError('靶车状态长度/CRC错误')
    if data[0] == 0x5a and data[1:3] == bytes([15, 2]):
        yaw, pitch = struct.unpack_from('<ff', data, 6)
        if not math.isfinite(yaw) or not math.isfinite(pitch) or data[5] > 4:
            raise ValueError('靶车反馈字段无效')
        return 'stm', dict(seq=data[3], state=data[5], yaw=yaw, pitch=pitch, temp=data[14], error=data[15])
    if data[0] != 0xaa or data[1] != 13 or data[16] != 0x55 or crc16(data[1:15]) & 255 != data[15]:
        raise ValueError('靶车回传帧校验错误')
    if data[2] == 6:
        p,v,t,fail = struct.unpack_from('<hhhH', data, 4)
        return 'motor', dict(seq=data[3], pos=p*.01, vel=v*.01, tau=t*.001, can_fail=fail,
                             safety=data[12], drops=data[13], token=data[14])
    if data[2] == 5:
        return 'param', dict(seq=data[3], rid=data[4], ok=data[5], raw=data[6:10].hex())
    raise ValueError('未知靶车回传类型')
