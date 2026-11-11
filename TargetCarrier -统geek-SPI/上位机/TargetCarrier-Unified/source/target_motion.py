"""Same six trajectory formulas/limits as target esp32_master/motion_profile.c."""
import math
import struct


def clamp(v, lo, hi): return max(lo, min(hi, v))


class TargetMotion:
    NAMES = ['STATIC', 'SINE', 'SWEEP', 'CIRCLE', 'FIGURE8', 'MANUAL']
    def __init__(self):
        self.profile = 0; self.t = 0.; self.yaw = self.pitch = 0.
        self.center_yaw = self.center_pitch = 0.
        self.manual_yaw = self.manual_pitch = 0.
        self.sine_amp = 10.; self.period = 6.
        self.sweep_amp = 60.; self.sweep_speed = 20.; self.sweep_pos = -60.
        self.sweep_dir = 1; self.dwell_until = 0.
        self.velocity = self.cruise = False
        self.yaw_rate = self.pitch_rate = 0.
        self.cruise_speed = 10.; self.cruise_dir = 1

    def select(self, profile):
        self.profile = profile; self.t = 0
        self.center_yaw, self.center_pitch = self.yaw, self.pitch
        self.velocity = self.cruise = False
        self.yaw_rate = self.pitch_rate = 0
        if profile in (0,5): self.manual_yaw, self.manual_pitch = self.yaw, self.pitch
        if profile == 2:
            self.sweep_pos = clamp(self.yaw,-self.sweep_amp,self.sweep_amp)
            self.sweep_dir = -1 if self.sweep_pos > 0 else 1
            self.dwell_until = 0

    def hold(self, yaw=None, pitch=None):
        if yaw is not None: self.yaw = clamp(yaw,-180,180)
        if pitch is not None: self.pitch = clamp(pitch,-30,60)
        self.select(5)

    def angles(self, yaw, pitch):
        self.select(5); self.manual_yaw, self.manual_pitch = yaw,pitch

    def rates(self, yaw, pitch):
        self.select(5); self.velocity=True; self.yaw_rate,self.pitch_rate=yaw,pitch

    def step(self, dt):
        dt=clamp(dt,0,.5)
        # Match the original float32 time accumulator and integer-ms dwell.
        dt=struct.unpack('<f',struct.pack('<f',dt))[0]
        self.t=struct.unpack('<f',struct.pack('<f',self.t+dt))[0]
        if self.cruise:
            self.manual_yaw+=self.cruise_dir*self.cruise_speed*dt
            if self.manual_yaw>=self.sweep_amp: self.manual_yaw=self.sweep_amp;self.cruise_dir=-1
            elif self.manual_yaw<=-self.sweep_amp: self.manual_yaw=-self.sweep_amp;self.cruise_dir=1
        if self.velocity:
            self.manual_yaw=clamp(self.manual_yaw+self.yaw_rate*dt,-180,180)
            self.manual_pitch=clamp(self.manual_pitch+self.pitch_rate*dt,-30,60)
        yr=pr=0.
        if self.profile==0: y,p=self.manual_yaw,self.manual_pitch
        elif self.profile==5:
            dy=clamp(self.manual_yaw-self.yaw,-180*dt,180*dt)
            dp=clamp(self.manual_pitch-self.pitch,-180*dt,180*dt)
            y,p=self.yaw+dy,self.pitch+dp
            yr,pr=(dy/dt,dp/dt) if dt else (0,0)
        elif self.profile==1:
            w=2*math.pi/self.period
            y,p=self.center_yaw+self.sine_amp*math.sin(w*self.t),self.center_pitch
            yr=self.sine_amp*w*math.cos(w*self.t)
        elif self.profile==2:
            now_ms=int(self.t*1000)
            if self.dwell_until and now_ms<self.dwell_until: y=self.sweep_pos
            else:
                self.dwell_until=0;self.sweep_pos+=self.sweep_dir*self.sweep_speed*dt
                if self.sweep_pos>=self.sweep_amp:
                    self.sweep_pos=self.sweep_amp;self.sweep_dir=-1;self.dwell_until=now_ms+500
                elif self.sweep_pos<=-self.sweep_amp:
                    self.sweep_pos=-self.sweep_amp;self.sweep_dir=1;self.dwell_until=now_ms+500
                y=self.sweep_pos;yr=self.sweep_dir*self.sweep_speed
            p=self.center_pitch
        elif self.profile==3:
            w=2*math.pi/8
            y,p=self.center_yaw+20*math.cos(w*self.t),self.center_pitch+20*math.sin(w*self.t)
            yr,pr=-20*w*math.sin(w*self.t),20*w*math.cos(w*self.t)
        elif self.profile==4:
            w=2*math.pi/10
            y,p=self.center_yaw+40*math.sin(w*self.t),self.center_pitch+15*math.sin(2*w*self.t)
            yr,pr=40*w*math.cos(w*self.t),30*w*math.cos(2*w*self.t)
        else: raise ValueError('未知轨迹')
        if self.velocity: yr,pr=self.yaw_rate,self.pitch_rate
        elif self.cruise: yr,pr=self.cruise_dir*self.cruise_speed,0
        self.yaw,self.pitch=clamp(y,-180,180),clamp(p,-30,60)
        return self.yaw,self.pitch,clamp(yr,-180,180),clamp(pr,-180,180)
