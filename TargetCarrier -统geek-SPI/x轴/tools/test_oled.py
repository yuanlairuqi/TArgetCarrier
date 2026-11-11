"""Test OLED byte streams and pulse freshness without boards or displays."""
import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gcc', default='gcc')
    parser.add_argument('--root', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--stm32', type=Path)
    parser.add_argument('--esp', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    stm = args.stm32 or args.root/'野火stm'
    esp = args.esp or args.root/'esp'
    output = args.output or args.root/'verification/oled_host_test'
    compiler = shutil.which(args.gcc)
    if not compiler:
        parser.error('Native GCC required; supply --gcc, not ARM GCC')
    work = Path(tempfile.mkdtemp(prefix='targetcarrier-oled-tests-'))
    if not str(work).isascii():
        raise RuntimeError('Use an ASCII TEMP directory for the native toolchain')
    env = dict(os.environ)
    env['PATH'] = str(Path(compiler).parent)+os.pathsep+env.get('PATH','')
    results = []
    for label, directory, test, files in (
        ('stm_oled',stm/'App/Display',stm/'tests/test_oled_text.c',('oled_text.c','oled_text.h')),
        ('esp_oled',esp/'main',stm/'tests/test_oled_text.c',('oled_text.c','oled_text.h')),
        ('pulse_meter',esp/'main',esp/'tests/test_pulse_meter.c',('pulse_meter.h',))):
        source = work/label
        source.mkdir()
        shutil.copy2(test, source/'test.c')
        for name in files:
            shutil.copy2(directory/name, source/name)
        command = [compiler,'-std=c11','-Wall','-Wextra','-Werror','-I'+str(source),
                   str(source/'test.c'),*[str(source/n) for n in files if n.endswith('.c')],
                   '-o',str(source/'test.exe')]
        build = subprocess.run(command, capture_output=True, env=env)
        if build.returncode:
            raise RuntimeError((build.stdout+build.stderr).decode('utf-8',errors='replace'))
        run = subprocess.run([str(source/'test.exe')], capture_output=True, env=env)
        log = (run.stdout+run.stderr).decode('utf-8',errors='replace')
        if run.returncode:
            raise RuntimeError(label+' failed:\n'+log)
        results.append({'name':label,'groups':log.count('PASS '),'log':log})
    output.mkdir(parents=True,exist_ok=True)
    report = {'passed':True,'groups':sum(r['groups'] for r in results),
              'hardware_tested':False,'native_workdir':str(work),'tests':results}
    (output/'RESULT.json').write_text(json.dumps(report,indent=2),encoding='utf-8')
    print(f'PASS: {report["groups"]} OLED/frequency groups; {output}')


if __name__ == '__main__':
    main()
