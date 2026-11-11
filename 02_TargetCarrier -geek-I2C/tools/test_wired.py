from pathlib import Path
import os,subprocess,sys,json

import argparse
p=argparse.ArgumentParser(description='Production-code protocol, driver and business model tests')
p.add_argument('--out',type=Path,required=True);p.add_argument('--gcc',type=Path,default=Path('D:/mingw/code/mingw64/bin/gcc.exe'));a=p.parse_args()
R=Path(__file__).resolve().parents[1];V=json.loads((R/'wired_transport/variant.json').read_text(encoding='utf8'))['variant']
D=R/'wired_transport';T=D/'tests';O=a.out.resolve();O.mkdir(parents=True,exist_ok=True);gcc=a.gcc;env=dict(os.environ);env['PATH']=str(gcc.parent)+';'+env['PATH'];results=[]

def run(label,src,inc,defines=()):
 exe=O/(label+'.exe');cmd=[str(gcc),'-std=c11','-Wall','-Wextra','-Wno-unused-function','-Wno-unused-variable','-Wno-unused-parameter','-Wno-pointer-to-int-cast','-Wno-sign-compare','-Wno-misleading-indentation',*['-D'+x for x in defines],*['-I'+str(x) for x in inc],*[str(x) for x in src],'-o',str(exe)]
 with (O/(label+'.log')).open('wb') as log:
  subprocess.run(cmd,env=env,stdout=log,stderr=subprocess.STDOUT,check=True);subprocess.run([str(exe)],env=env,stdout=log,stderr=subprocess.STDOUT,check=True)
 results.append(label);(O/'results.json').write_text(json.dumps(results,indent=2),encoding='utf8');print(V,label,'PASS',flush=True)
run('codec',[D/'tc_protocol.c',D/'wl_codec.c',T/'test_codec.c'],[D])
run('stm-driver',[D/'tc_protocol.c',T/'test_stm32.c'],[T/'mocks',D])
run('esp-driver',[D/'tc_protocol.c',T/'test_esp_bus.c'],[T/'esp_mocks',D])
stm=R/'x轴/野火stm';esp=R/'x轴/esp_无线从机_20261022';inc=[stm/x for x in ['tests/stubs','Core/Inc','App/Board','App/Motion','App/Link']]+[esp/'main',D]
run('x-business',[T/'test_x_business.c',D/'tc_protocol.c',D/'wl_codec.c',stm/'App/Link/robot_uart.c',stm/'App/Link/robot_wire.c',stm/'App/Link/robot_peer.c',stm/'App/Motion/motor_motion.c',stm/'App/Motion/motion_profile.c',esp/'main/robot_bridge.c'],inc,['MOTOR_HOST_TEST','ROBOT_UART_HOST_TEST','MOTOR_CONTINUOUS_ENABLED=1','MOTOR_STARTUP_ONESHOT_ENABLED=0'])
vehicle=R/'靶车/靶车/firmware/lib';run('target-guard',[vehicle/'linkcore/test/test_linkcore.c',*sorted((vehicle/'linkcore').glob('*.c'))],[vehicle/'linkcore/include',vehicle/'protocol'])
print('All production-source host tests passed; no physical hardware simulated as evidence')
