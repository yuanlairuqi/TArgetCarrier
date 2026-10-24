"""Vue desktop shell. The existing BLE controller remains the only GATT owner."""
import csv
from copy import deepcopy
import io
import json
import logging
import math
import queue
import sys
import threading
import time
from collections import deque
from dataclasses import asdict
from logging.handlers import RotatingFileHandler
from pathlib import Path
from usb_transport import CombinedController as Controller
from protocol import command, link_description

VERSION = '3.0-ramp'

class Bridge:
    def __init__(self, controller=None):
        self._events = queue.Queue(maxsize=1500)
        self._controller = controller or Controller(self._events)
        self._run_recipe=None
        self._profile_stop_requested=False
        if controller is None:self._controller.ramp=True
        self._lock = threading.RLock()
        self._window = None
        self._raw = deque(maxlen=120)
        self._samples = deque(maxlen=1200)
        self._last_focus = 0
        self._closing = False
        self._frozen_samples = None
        self._frozen_raw = None
        self._frozen_capture = None
        self._pulse_frozen = False
        self._state = dict(version=VERSION, connection='未连接设备', connected=False, available=False,
            busy=False, running=False, devices=[], status=None, health={}, capture=None,
            pulse_state='等待采集数据', notice='先连接 S3，再确认 STM32 状态',
            transport=None, session_started_at=0, link='BLE / USB 未连接', hint='串口恢复后重新确认停止，不自动恢复运动。')

    def _log(self, kind, value):
        self._raw.append(dict(time=time.strftime('%H:%M:%S'), kind=kind, text=str(value)))

    def _drain(self):
        for _ in range(1500):
            try: kind, value = self._events.get_nowait()
            except queue.Empty: break
            if kind in ('connection','notice','pulse_state','busy','available'):
                self._state[kind] = value
                if kind in ('connection','notice'): self._log('事件', value)
                if kind == 'available' and not value: self._state['running'] = False
                if kind == 'available' and value and not self._state['running']:self._profile_stop_requested=False
            elif kind == 'devices':
                self._state['devices'] = [dict(key=k, name=n, rssi=r, transport='USB' if k.startswith('usb:') else 'BLE') for k,n,r in value]
            elif kind == 'reset':
                self._state.update(connected=False, status=None, capture=None, available=False, running=False, health={}, transport=None, session_started_at=time.time())
                self._state['pulse_state']='新会话：等待首段 GPIO 采样'
                self._samples.append(dict(at=time.time(), gap=True))
            elif kind == 'status':
                self._state['connected'] = True
                self._state['status'] = dict(asdict(value), voltage=value.voltage, demo=value.demo, uart=value.uart, online=value.online)
                self._state['link'], self._state['hint'] = link_description(value)
                if self._state.get('transport'):
                    self._state['link'] = self._state['link'].replace('BLE 正常', 'USB → GEEK → ESP-NOW 状态已接收')
                self._samples.append(dict(at=time.time(), **self._state['status']))
            elif kind == 'health': self._state['health'] = value
            elif kind == 'pulse':
                value.setdefault('received_at',time.time());value['historical']=False
                self._state['capture'] = value
            elif kind == 'transport':
                self._state['transport']=value
                self._state['link']='USB → GEEK → ESP-NOW · '+str(value.get('diagnostic') or '等待从机应答')
            elif kind in ('tx','raw'): self._log('TX' if kind == 'tx' else 'RX', value.hex(' ').upper())
            elif kind == 'disconnected':
                self._state.update(connected=False, available=False, running=False, status=None,
                    transport=None, link='连接已断开', hint='STM32 当前状态未知，请重新连接 S3。')
                self._state['pulse_state']='已断线：保留历史 GPIO 采样，当前状态未知'
                if self._state['capture']:self._state['capture']=dict(self._state['capture'],historical=True)
                if self._frozen_capture:self._frozen_capture=dict(self._frozen_capture,historical=True)
                self._samples.append(dict(at=time.time(), gap=True))

    def refresh(self, focused=False):
        with self._lock:
            focused = focused is True
            if focused and not self._closing:
                self._last_focus = time.monotonic()
                self._controller.call('alive')
            elif self._state['running']:
                self._controller.call('stop')
                self._state['running'] = False
            self._drain()
            return deepcopy(dict(self._state, raw=list(self._raw), samples=list(self._samples)[-320:]))

    def action(self, name, key=None):
        with self._lock:
            if name not in ('scan','scan_usb','connect','disconnect','stop','soft_stop','resync'): raise ValueError('未知操作')
            if self._closing: return False
            if name in ('soft_stop','stop','disconnect','resync'):self._profile_stop_requested=True
            if name == 'connect':
                if key not in [d['key'] for d in self._state['devices']]: raise ValueError('请先搜索并选择设备')
                self._state['busy'] = True
                self._controller.call(name, key)
            else:
                if name in ('scan','scan_usb'): self._state['busy'] = True
                if name in ('disconnect','stop','resync'):
                    self._state['running'] = self._state['available'] = False
                self._controller.call(name)
            return True

    def move(self, motion, speed, mode, forward, reverse, accel=None, decel=None):
        with self._lock:
            values = (motion, speed, mode, forward, reverse)
            if not all(type(v) in (int,float) and math.isfinite(v) and int(v) == v for v in values):
                raise ValueError('参数必须是有限整数')
            motion, speed, mode, forward, reverse = map(int, values)
            command(motion, speed, 1, mode, forward, reverse, accel, decel)
            if motion == 0: raise ValueError('停止请使用停止按钮')
            if self._closing or time.monotonic() - self._last_focus > .6: raise ValueError('窗口未激活，请重新开始')
            if self._profile_stop_requested or not self._state['available'] or self._state['running']: raise ValueError('请等待停止确认和 STM32 就绪')
            self._run_recipe=(motion,mode,forward,reverse)
            if accel is None:self._controller.call('move',motion,speed,mode,forward,reverse)
            else:self._controller.call('move',motion,speed,mode,forward,reverse,accel,decel)
            self._state['running'] = True
            return True


    def update_profile(self,speed,accel,decel):
        with self._lock:
            if not self._run_recipe or not self._state['running'] or not self._state['available']:
                raise ValueError('当前没有可更新的运动')
            if self._profile_stop_requested or getattr(self._controller,'soft_stopping',False):raise ValueError('减速停止中不能重新启动')
            if time.monotonic()-self._last_focus>.6:raise ValueError('窗口未激活')
            motion,mode,f,r=self._run_recipe
            if not all(type(v) is int for v in (speed,accel,decel)):raise ValueError('请输入整数')
            command(motion,speed,1,mode,f,r,accel,decel)
            self._controller.call('move',motion,speed,mode,f,r,accel,decel)
            return True

    def clear(self, kind):
        with self._lock:
            if kind == 'raw':
                self._raw.clear(); self._samples.clear()
                if self._frozen_samples is not None: self._frozen_samples = []
                if self._frozen_raw is not None: self._frozen_raw = []
            elif kind == 'pulse': self._state['capture'] = self._frozen_capture = None
            else: raise ValueError('未知记录类型')
        return True

    def freeze(self, kind, active):
        with self._lock:
            self._drain()
            if type(active) is not bool: raise ValueError('无效冻结状态')
            if kind == 'status':
                self._frozen_samples = deepcopy(list(self._samples)) if active else None
                self._frozen_raw = deepcopy(list(self._raw)) if active else None
            elif kind == 'pulse':
                self._pulse_frozen = active
                self._frozen_capture = deepcopy(self._state['capture']) if active else None
            else: raise ValueError('未知冻结类型')
        return deepcopy(dict(capture=self._frozen_capture, samples=self._frozen_samples, raw=self._frozen_raw if self._frozen_raw is not None else list(self._raw)))

    def copy_text(self, kind):
        with self._lock:
            if kind == 'raw':
                self._drain()
                rows = self._frozen_raw if self._frozen_raw is not None else self._raw
                return '\n'.join(f"{r['time']} {r['kind']} {r['text']}" for r in rows)
            if kind != 'pulse': raise ValueError('未知复制类型')
            return self._export_text(kind)

    def _export_text(self, kind):
        self._drain()
        output = io.StringIO(newline='')
        writer = csv.writer(output)
        if kind == 'status':
            writer.writerow(['time','speed_pct','voltage_V','motion','phase','flags','error','esp_ack','stm_ack'])
            for s in self._samples if self._frozen_samples is None else self._frozen_samples:
                known = not s.get('uart') or s.get('online')
                writer.writerow([s['at'],s.get('speed',''),s.get('voltage',''),s.get('motion','') if known else '',
                    s.get('phase','') if known else '',s.get('flags',''),s.get('error',''),s.get('ack',''),s.get('stm_ack','')])
        elif kind == 'pulse':
            c = self._frozen_capture if self._pulse_frozen else self._state['capture']
            if not c: raise ValueError('暂无完整采样')
            writer.writerow(['# capture',c['id'],'flags',c['flags'],'initial',c['initial'],'hz',c['hz'],'window_ticks',c['duration']])
            writer.writerow(['# source',c.get('source','S3 GPIO capture'),'node',c.get('node','BLE'),'boot',c.get('boot',''),'start_ms',c.get('start_ms',''),'received_at',c.get('received_at',''),'historical',c.get('historical',False)])
            writer.writerow(['channel','time_ticks','time_ms','level'])
            for tick,tag in c['edges']: writer.writerow(['DIR' if tag&1 else 'PUL',tick,tick*1000/c['hz'] if c['hz'] else '',tag>>1])
        elif kind == 'diagnostics':
            return json.dumps(dict(version=VERSION,time=time.strftime('%Y-%m-%d %H:%M:%S'),
                connection=self._state['connection'],link=self._state['link'],hint=self._state['hint'],
                health=self._state['health'],status=self._state['status'],transport=self._state.get('transport'),recent=list(self._raw)),ensure_ascii=False,indent=2)
        else: raise ValueError('未知导出类型')
        return output.getvalue()

    def export(self, kind):
        self.action('stop')
        with self._lock: text = self._export_text(kind)
        import webview
        extension = 'txt' if kind == 'diagnostics' else 'csv'
        paths = self._window.create_file_dialog(webview.FileDialog.SAVE, save_filename=f'{kind}.{extension}')
        if not paths: return False
        path = paths if isinstance(paths,str) else paths[0]
        Path(path).write_text(text,encoding='utf-8-sig')
        return True

    def _shutdown(self):
        self._closing = True
        self._controller.call('stop')
        self._controller.call('disconnect')
        time.sleep(1)

def main():
    import webview
    base = Path(sys.executable).parent if getattr(sys,'frozen',False) else Path(__file__).resolve().parent
    asset_base = Path(getattr(sys,'_MEIPASS',base))
    (base/'logs').mkdir(exist_ok=True)
    handler = RotatingFileHandler(base/'logs'/'connection.log',maxBytes=1024*1024,backupCount=3,encoding='utf8')
    handler.setFormatter(logging.Formatter('%(asctime)s %(levelname)s %(message)s'))
    logger=logging.getLogger('targetcarrier.link');logger.setLevel(logging.INFO);logger.addHandler(handler)
    logger.info('PC %s Vue BLE/USB desktop started', VERSION)
    bridge=Bridge()
    window=webview.create_window('TargetCarrier · 电机控制台 3.0 · BLE / USB',url=str(asset_base/'web'/'index.html'),
        js_api=bridge,width=1220,height=820,min_size=(920,640),background_color='#f5f7fb',text_select=True)
    bridge._window=window
    window.events.closing += bridge._shutdown
    def smoke():
        if '--ui-smoke' not in sys.argv: return
        time.sleep(3)
        info=window.evaluate_js('({title:document.title, vue:!!document.querySelector(".workspace"), ready:!!window.__tcReady})')
        if info.get('vue') and info.get('ready'):
            Path('vue-ui-smoke.json').write_text(json.dumps(info),encoding='utf8')
        window.destroy()
    window.events.loaded += lambda: threading.Thread(target=smoke,daemon=True).start()
    webview.start(gui='edgechromium',private_mode=True)

if __name__=='__main__': main()
