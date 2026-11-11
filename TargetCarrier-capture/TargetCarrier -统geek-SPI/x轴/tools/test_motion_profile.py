"""Run the real motion/peer/UART/ESP bridge models without flashing hardware."""
from pathlib import Path
import argparse
import os
import shutil
import subprocess
import tempfile

def main():
    root=Path(__file__).resolve().parents[1]
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--stm',type=Path,default=root/'野火stm')
    p.add_argument('--esp',type=Path,default=root/'esp_无线从机_20261022')
    p.add_argument('--gcc',default='gcc')
    p.add_argument('--output',type=Path,default=root/'verification/motion_profile_tests')
    a=p.parse_args();gcc=shutil.which(a.gcc)
    if not gcc:p.error('Native GCC required, not ARM GCC')
    work=Path(tempfile.mkdtemp(prefix='tc-profile-'))
    if not str(work).isascii():p.error('ASCII TEMP directory required')
    for name,source in [('motion',a.stm/'App/Motion'),('link',a.stm/'App/Link'),('stubs',a.stm/'tests/stubs'),('esp',a.esp/'main')]:
        (work/name).mkdir()
        for f in source.glob('*'):
            if f.is_file() and f.suffix in ('.c','.h'):shutil.copy2(f,work/name/f.name)
    shutil.copy2(a.stm/'tests/test_profile_link.c',work/'test.c')
    sources=['test.c','motion/motor_motion.c','motion/motion_profile.c','link/robot_peer.c','link/robot_wire.c','esp/robot_control.c','esp/robot_bridge.c']
    env=dict(os.environ,PATH=str(Path(gcc).parent)+os.pathsep+os.environ.get('PATH',''))
    cmd=[gcc,'-std=c11','-Wall','-Wextra','-Werror','-DMOTOR_HOST_TEST',
         *['-I'+str(work/n) for n in ('motion','link','stubs','esp')],*[str(work/s) for s in sources],'-o',str(work/'test.exe')]
    build=subprocess.run(cmd,capture_output=True,env=env)
    if build.returncode:raise RuntimeError((build.stdout+build.stderr).decode('utf8',errors='replace'))
    result=subprocess.run([str(work/'test.exe')],capture_output=True,env=env)
    log=(result.stdout+result.stderr).decode('utf8',errors='replace')
    a.output.mkdir(parents=True,exist_ok=True);(a.output/'profile-tests.log').write_text(log,encoding='utf8')
    print(log,end='')
    if result.returncode:raise RuntimeError('Motion model failed')
    print('No hardware was accessed.')
if __name__=='__main__':main()
