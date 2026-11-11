from pathlib import Path
import subprocess,os,sys
R=Path(__file__).resolve().parents[1];B=Path(sys.argv[1]).resolve();B.mkdir(parents=True,exist_ok=True)
gcc=Path(os.environ.get('TC_HOST_GCC','D:/mingw/code/mingw64/bin/gcc.exe'));env=dict(os.environ);env['PATH']=str(gcc.parent)+';'+env['PATH'];
def run(c):subprocess.run(list(map(str,c)),cwd=B,env=env,check=True)
run([gcc,'-std=c11','-Wall','-Wextra','-Werror','-Wno-sign-compare','-I'+str(R/'core'),*[R/'core'/n for n in ('tc_protocol.c','tc_params.c','tc_wave.c')],R/'tests/test_core.c','-o',B/'test_core.exe']);run([B/'test_core.exe'])
for n in (1,2,3):
 exe=B/f'test_bus_{n}.exe';run([gcc,'-std=c11','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-function','-Wno-pointer-to-int-cast','-Wno-sign-compare','-I'+str(R/'tests/mocks'),'-I'+str(R/'core'),'-I'+str(R/'config'),'-I'+str(R/'platform'),'-DTC_BACKEND='+str(n),*[R/'core'/n for n in ('tc_protocol.c','tc_params.c','tc_wave.c')],R/'tests/test_stm32.c','-o',exe]);run([exe])
run([sys.executable,R/'tests/test_pc.py'])
run([gcc,'-shared','-std=c11','-I'+str(R/'core'),*[R/'core'/n for n in ('tc_protocol.c','tc_params.c','tc_wave.c')],R/'tests/sim_service.c','-o',B/'sim_service.dll'])
run([sys.executable,R/'tests/test_client_sim.py'])
for n in (1,2,3):
 exe=B/f'test_esp_bus_{n}.exe';run([gcc,'-std=c11','-Wall','-Wextra','-Wno-unused-variable','-Wno-unused-function','-Wno-unused-parameter','-Wno-sign-compare','-I'+str(R/'tests/esp_mocks'),'-I'+str(R/'core'),'-I'+str(R/'config'),'-I'+str(R/'platform'),'-DTC_BACKEND='+str(n),*[R/'core'/s for s in ('tc_protocol.c','tc_params.c','tc_wave.c')],R/'tests/test_esp_bus.c','-o',exe]);run([exe])
print('All core/STM32 adapter/PC tests passed. Hardware not tested.')
