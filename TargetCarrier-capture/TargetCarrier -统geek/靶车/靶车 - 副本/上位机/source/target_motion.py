"""Explicit MIT speed-only requests and position trajectories.
Continuous speed never accumulates position on the PC. STM32 owns its ramp.
Yaw is an orientation; normalization at transport is not a travel limit.
"""
import math
def clamp(v,lo,hi):return max(lo,min(hi,v))
class TargetMotion:
    NAMES=['STATIC','SINE','SWEEP','CIRCLE','FIGURE8','MANUAL']
    def __init__(self):
        self.profile=0;self.t=0.;self.yaw=self.pitch=0.
        self.center_yaw=self.center_pitch=0.;self.manual_yaw=self.manual_pitch=0.
        self.sine_amp=10.;self.period=6.
        self.sweep_amp=60.;self.sweep_speed=20.;self.sweep_pos=0.
        self.sweep_dir=1;self.sweep_velocity=0.;self.dwell_until=0.
        self.velocity=self.cruise=False;self.yaw_rate=self.pitch_rate=0.
        self.cruise_speed=10.;self.cruise_dir=1
    def select(self,profile):
        self.profile=profile;self.t=0.
        self.center_yaw,self.center_pitch=self.yaw,self.pitch
        self.velocity=self.cruise=False;self.yaw_rate=self.pitch_rate=0.
        self.manual_yaw,self.manual_pitch=self.yaw,self.pitch
        self.sweep_pos=self.yaw;self.sweep_dir=1;self.sweep_velocity=0.;self.dwell_until=0.
        if profile==3:self.center_yaw-=20.
    def hold(self,yaw=None,pitch=None):
        if yaw is not None:self.yaw=yaw
        if pitch is not None:self.pitch=clamp(pitch,-30,60)
        self.select(5)
    def angles(self,yaw,pitch):
        self.select(5)
        self.manual_yaw=self.yaw+(yaw-self.yaw+180.)%360.-180.
        self.manual_pitch=pitch
    def rates(self,yaw,pitch):
        self.select(5);self.velocity=True;self.yaw_rate,self.pitch_rate=yaw,pitch
    def _sweep(self,dt,speed):
        lo,hi=self.center_yaw-self.sweep_amp,self.center_yaw+self.sweep_amp
        bound=hi if self.sweep_dir>0 else lo
        distance=abs(bound-self.sweep_pos)
        wanted=self.sweep_dir*min(speed,math.sqrt(2*30.*distance))
        self.sweep_velocity+=clamp(wanted-self.sweep_velocity,-30*dt,30*dt)
        step=self.sweep_velocity*dt
        if (self.sweep_dir>0 and self.sweep_pos+step>=hi or self.sweep_dir<0 and self.sweep_pos+step<=lo):
            self.sweep_pos=bound;self.sweep_velocity=0.;self.sweep_dir*=-1
        else:self.sweep_pos+=step
        return self.sweep_pos,self.sweep_velocity
    def step(self,dt):
        dt=clamp(dt,0,.25);self.t+=dt
        if self.velocity:return self.yaw,self.pitch,self.yaw_rate,self.pitch_rate
        yr=pr=0.
        if self.cruise:y,yr=self._sweep(dt,self.cruise_speed);p=self.center_pitch
        elif self.profile in (0,5):
            dy=clamp(self.manual_yaw-self.yaw,-90*dt,90*dt)
            dp=clamp(self.manual_pitch-self.pitch,-60*dt,60*dt)
            y,p=self.yaw+dy,self.pitch+dp
            if dt:yr,pr=dy/dt,dp/dt
        elif self.profile==1:
            w=2*math.pi/self.period
            y,p=self.center_yaw+self.sine_amp*math.sin(w*self.t),self.center_pitch
            yr=self.sine_amp*w*math.cos(w*self.t)
        elif self.profile==2:y,yr=self._sweep(dt,self.sweep_speed);p=self.center_pitch
        elif self.profile==3:
            w=2*math.pi/8
            y,p=self.center_yaw+20*math.cos(w*self.t),self.center_pitch+20*math.sin(w*self.t)
            yr,pr=-20*w*math.sin(w*self.t),20*w*math.cos(w*self.t)
        elif self.profile==4:
            w=2*math.pi/10
            y,p=self.center_yaw+40*math.sin(w*self.t),self.center_pitch+15*math.sin(2*w*self.t)
            yr,pr=40*w*math.cos(w*self.t),30*w*math.cos(2*w*self.t)
        else:raise ValueError('未知轨迹')
        self.yaw,self.pitch=y,clamp(p,-30,60)
        return self.yaw,self.pitch,clamp(yr,-90,90),clamp(pr,-60,60)
