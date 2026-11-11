
"""Compile the real BLE handler and mutex/time snapshot with deterministic host callbacks; no hardware."""
from pathlib import Path
import argparse,os,shutil,subprocess,tempfile
p=argparse.ArgumentParser(description=__doc__);p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--gcc',default='gcc');a=p.parse_args()
gcc=shutil.which(a.gcc)
if not gcc:p.error('Native GCC required')
s=(a.source/'main/main.c').read_text(encoding='utf8')
i=s.index('static int access_command(');j=s.index('static const struct ble_gatt_svc_def services[]',i);access=s[i:j]
i=s.index('static void telemetry_task(');j=s.index('        apply_limit();',i)
time=s[i:j].replace('static void telemetry_task(void *arg)','static uint32_t capture_tick_time(void)').replace('(void)arg;','')+'        return now;\n    }\n}\n'
template=Path(__file__).with_name('ble_access_timing.c.in').read_text(encoding='utf8')
with tempfile.TemporaryDirectory(prefix='tc-ble-timing-') as d:
 d=Path(d)
 if not str(d).isascii():raise RuntimeError('ASCII TEMP required')
 for rel in ['main/now_protocol.c','main/now_protocol.h','main/robot_control.c','main/robot_control.h','shared/robot_wire.h']:
  shutil.copy2(a.source/rel,d/Path(rel).name)
 (d/'test.c').write_text(template.replace('@ACCESS@',access).replace('@TIME@',time),encoding='utf8')
 env=dict(os.environ);env['PATH']=str(Path(gcc).parent)+os.pathsep+env.get('PATH','')
 subprocess.run([gcc,'-std=c11','-I'+str(d),str(d/'test.c'),str(d/'now_protocol.c'),str(d/'robot_control.c'),'-o',str(d/'test.exe')],env=env,check=True)
 for case in ['gatt','time']:subprocess.run([str(d/'test.exe'),case],env=env,check=True)
