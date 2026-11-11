"""Unified Vue shell: original x-axis bridge plus independent legacy target owner."""
import csv
import io
import json
import logging
import sys
import threading
import time
from logging.handlers import RotatingFileHandler
from pathlib import Path

from wave_client import Recorder
from desktop import Bridge
from target_ble import BleTargetAdapter
from target_usb import UsbTargetAdapter
from linear_program import LinearProgram


class UnifiedBridge(Bridge):
    def launch_portable(self):
        # Separate process/protocol; never commandeer an open legacy COM port.
        import subprocess
        if getattr(sys, 'frozen', False):
            exe = Path(sys.executable).parents[2] / 'TC-Portable' / 'TC-Portable.exe'
            if not exe.is_file():
                raise FileNotFoundError('缺少配套 TC-Portable/TC-Portable.exe')
            subprocess.Popen([str(exe)], cwd=str(exe.parent))
        else:
            script = Path(__file__).parent / 'portable' / 'portable_console.py'
            subprocess.Popen([sys.executable, str(script)], cwd=str(script.parent))
        return True

    def __init__(self, controller=None, target=None):
        super().__init__(controller)
        self._target = target or UsbTargetAdapter()
        self._recorder = Recorder()
        self._stop_all_request=None
        self._state['version'] = '6.0-dual-espnow'
        self._programs = {lane: LinearProgram(self, lane) for lane in ('x', 'target')}

    def refresh(self, focused=False):
        with self._lock:
            self._target.alive(focused is True and not self._closing)
            super().refresh(focused)
            for program in self._programs.values():
                program.tick(focused is True)
            from copy import deepcopy
            state = deepcopy(dict(self._state, raw=list(self._raw), samples=list(self._samples)[-320:]))
            state['target'] = self._target.snapshot()
            state['recorder'] = self._recorder.snapshot()
            state['programs'] = {lane: program.snapshot() for lane, program in self._programs.items()}
            state['unified'] = dict(stage=3, automatic_joint_motion=False, linear_program=True, parallel_programs=True,
                device_map={'x':1,'target':2},fault_policy='保留已有程序故障联停')
            if self._stop_all_request is not None:
                request=self._stop_all_request
                xfresh=(self._status_revision>request['x_revision'] and getattr(getattr(self._controller,'channel',None),'stop_seq',None)!=request['x_stop_seq'] and self._programs['x']._x_stopped())
                target_state=state['target'];tfresh=getattr(self._target,'motor_at',None)
                tconfirmed=bool(target_state.get('controller_stop_confirmed') and tfresh is not None and tfresh>request['at'])
                state['all_stop']=dict(x='STM32反馈已确认' if xfresh else '停止未确认',
                    target='控制器失能/零速反馈已确认' if tconfirmed else '停止未确认',
                    confirmed=bool(xfresh and tconfirmed),physical_stop_confirmed=False)
            return state

    def record_attach(self, port):
        self._recorder.attach(str(port));return True

    def record_action(self, op):
        return self._recorder.action(int(op))

    def record_list(self):
        return self._recorder.list_history()

    def record_note(self, ident, text):
        return self._recorder.note(int(ident),text)

    def record_delete(self, ident):
        return self._recorder.delete(int(ident))

    def record_download(self, ident):
        import webview
        paths=self._window.create_file_dialog(webview.FileDialog.SAVE,save_filename=f'TC-{int(ident):08x}.twl')
        if not paths:return False
        return self._recorder.download(int(ident),paths if isinstance(paths,str) else paths[0])

    def record_local(self, operation, node=None, start=None, end=None, recovery=False):
        import webview
        from datetime import datetime
        def stamp(value):
            if not value:return None
            return datetime.fromisoformat(str(value)).timestamp()*1000
        source=self._window.create_file_dialog(webview.FileDialog.OPEN,allow_multiple=True,file_types=('录波原始文件 (*.twl)','所有文件 (*.*)'))
        if not source:return False
        path=source if isinstance(source,str) else list(source)
        filters=dict(node=int(node) if node else None,start=stamp(start),end=stamp(end),recovery=recovery is True)
        if operation=='replay':return self._recorder.replay_file(path,**filters)
        if operation!='export':raise ValueError('未知历史操作')
        target=self._window.create_file_dialog(webview.FileDialog.SAVE,save_filename='TC-wave.csv')
        if not target:return False
        return self._recorder.export_file(path,target if isinstance(target,str) else target[0],**filters)

    def _lane(self, lane):
        if not isinstance(lane, str) or lane not in self._programs:
            raise ValueError('请选择 x 或 target 程序线')
        return self._programs[lane]

    def program_edit(self, lane, operation, value=None, ident=None):
        with self._lock:
            if self._closing:
                return False
            return self._lane(lane).edit(operation, value, ident)

    def program_start(self, lane, generation=None):
        with self._lock:
            program = self._lane(lane)
            self._check_generation(program, generation)
            self._drain()
            return program.start()

    def _check_generation(self, program, generation):
        if type(generation) is not int or generation != program.generation:
            raise ValueError('启动请求已被停止操作作废，请重新点击启动')

    def program_start_all(self, generations):
        with self._lock:
            if not isinstance(generations, dict):
                raise ValueError('程序会话无效')
            self._drain()
            selected = [p for p in self._programs.values() if p.rows]
            if not selected:
                raise ValueError('请先添加动作模块')
            for program in selected:
                self._check_generation(program, generations.get(program.lane))
                program.check_start()
            started = []
            try:
                for program in selected:
                    program.start()
                    started.append(program)
            except Exception:
                for program in started:
                    program.cancel('并行启动条件发生变化')
                raise
            return True

    def program_stop(self, lane):
        with self._lock:
            self._lane(lane).cancel('用户停止本条程序线')
            if lane == 'x':
                return super().action('stop')
            return self._target.stop('用户停止靶车程序线')

    def _program_fault(self, reason):
        for program in self._programs.values():
            program.cancel('故障联停：' + reason)
        self._stop_devices()

    def _program_move(self, step):
        return super().move(step['direction'], step['speed'], 0, 0, 0, step['accel'], step['decel'])

    def _program_x_stop(self):
        return super().action('soft_stop')

    def action(self, name, key=None):
        with self._lock:
            program = self._programs['x']
            if name in ('stop', 'soft_stop', 'disconnect', 'resync'):
                program.cancel('手动操作停止 x 轴程序线')
            elif program.active:
                raise ValueError('x 轴程序运行中不能更换连接，请先停止该程序线')
            if name=='connect' and isinstance(key,str) and key.startswith('usb:'):
                if key not in [d['key'] for d in self._state['devices']]:raise ValueError('请先搜索 GEEK USB')
                self._target.connect_usb(key)
            if name=='disconnect' and getattr(self._target,'usb_port',None):self._target.stop('GEEK断开',disconnect=True)
            return super().action(name, key)

    def move(self, *args):
        with self._lock:
            if self._programs['x'].active:
                raise ValueError('x 轴程序运行中请追加模块；手动运动需先停止该程序线')
            return super().move(*args)

    def update_profile(self, *args):
        with self._lock:
            if self._programs['x'].active:
                raise ValueError('当前模块参数已锁定，请修改待执行模块')
            return super().update_profile(*args)

    def target_scan(self):
        with self._lock:
            if self._programs['target'].active:
                raise ValueError('请先停止靶车程序线')
            return self._target.scan()

    def target_connect(self, address):
        with self._lock:
            if self._closing:
                return False
            if self._programs['target'].active:
                raise ValueError('靶车程序运行中不能更换连接')
            if getattr(self._target,'usb_port',None):return self._target.connect_usb(self._target.usb_port)
            return self._target.connect(address)

    def target_command(self, name, values=None):
        with self._lock:
            if self._closing:
                return False
            program = self._programs['target']
            if name in ('stop', 'disable', 'disconnect'):
                program.cancel('手动操作停止靶车程序线')
            elif program.active:
                raise ValueError('靶车程序运行中请追加模块；手动控制需先停止该程序线')
            return self._target.command(name, values)

    def stop_all(self):
        with self._lock:
            for program in self._programs.values():
                program.cancel('全部停止')
            self._stop_all_request=dict(at=time.monotonic(),x_revision=self._status_revision,x_stop_seq=getattr(getattr(self._controller,'channel',None),'stop_seq',None))
            return self._stop_devices()

    def _stop_devices(self):
        # Both requests are nonblocking and keep their existing transport owners.
        results = {}
        try:
            results['x_axis_requested'] = super().action('stop')
        except Exception as exc:
            results['x_axis_error'] = str(exc)
        finally:
            results['target_requested'] = self._target.stop('全部停止')
        return results

    def export_unified(self, kind='diagnostics'):
        self.stop_all()
        state = self.refresh(False)
        if kind == 'diagnostics':
            text = json.dumps(state, ensure_ascii=False, indent=2)
            filename = 'TargetCarrier-unified-diagnostics.json'
        elif kind == 'target':
            stream = io.StringIO(newline='')
            writer = csv.writer(stream)
            writer.writerow(['# source', 'target BLE bridge; STM32/CAN feedback age recorded separately'])
            writer.writerow(['time', 'gap', 'target_yaw_deg', 'target_pitch_deg', 'forwarded_yaw_deg',
                             'motor_pos_rad', 'motor_vel_rad_s', 'motor_tau_Nm', 'master_en', 'master_estop', 'forwarded_age_ms'])
            for s in state['target']['samples']:
                writer.writerow([s.get(k, '') for k in ('at','gap','yaw','pitch','st_yaw','m_pos','m_vel','m_tau','en','estop','st_age')])
            text, filename = stream.getvalue(), 'TargetCarrier-target-data.csv'
        else:
            raise ValueError('未知导出类型')
        import webview
        paths = self._window.create_file_dialog(webview.FileDialog.SAVE, save_filename=filename)
        if not paths:
            return False
        Path(paths if isinstance(paths, str) else paths[0]).write_text(text, encoding='utf-8-sig')
        return True

    def _shutdown(self):
        self.stop_all()
        self._closing = True
        self._recorder.close()
        self._target.close()
        self._controller.call('disconnect')
        time.sleep(.8)


def main():
    import webview
    base = Path(sys.executable).parent if getattr(sys, 'frozen', False) else Path(__file__).resolve().parent
    assets = Path(getattr(sys, '_MEIPASS', base))
    (base / 'logs').mkdir(exist_ok=True)
    handler = RotatingFileHandler(base / 'logs' / 'connection.log', maxBytes=1024*1024, backupCount=3, encoding='utf-8')
    handler.setFormatter(logging.Formatter('%(asctime)s %(levelname)s %(message)s'))
    log = logging.getLogger('targetcarrier.link')
    log.setLevel(logging.INFO)
    log.addHandler(handler)
    log.info('TargetCarrier 6.0 dual ESP-NOW started: %s',sys.executable)
    bridge = UnifiedBridge()
    window = webview.create_window('TargetCarrier · GEEK 双从机控制台 6.0',
        url=str(assets / 'web' / 'index.html'), js_api=bridge, width=1340, height=900,
        min_size=(1000, 700), background_color='#f5f7fb', text_select=True)
    bridge._window = window
    window.events.closing += bridge._shutdown

    def smoke():
        if '--ui-smoke' not in sys.argv:
            return
        time.sleep(3)
        try:
            info = window.evaluate_js("({title:document.title,vue:!!document.querySelector('.workspace'),ready:!!window.__tcReady,overview:!!document.querySelector('.unified-overview'),nav:document.querySelectorAll('nav button').length,text:document.body.innerText})")
            window.evaluate_js("Array.from(document.querySelectorAll('nav button')).find(b=>b.innerText.includes('并行程序')).click()")
            time.sleep(.5)
            info['parallel'] = window.evaluate_js("({lanes:document.querySelectorAll('.program-lane').length,x:!!document.querySelector('.program-lane.x'),target:!!document.querySelector('.program-lane.target'),text:document.querySelector('.parallel-lanes')?.innerText})")
            Path('unified-ui-smoke.json').write_text(json.dumps(info, ensure_ascii=False, indent=2), encoding='utf-8')
        finally:
            window.destroy()
    window.events.loaded += lambda: threading.Thread(target=smoke, daemon=True).start()
    webview.start(gui='edgechromium', private_mode=True)


if __name__ == '__main__':
    main()
