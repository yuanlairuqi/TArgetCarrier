"""Build this STM32 project in an isolated ASCII path; copy artifacts back."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def manifest(root):
    return {p.relative_to(root).as_posix(): digest(p)
            for p in root.rglob('*') if p.is_file()
            and not any(x in ('build', '.git', '__pycache__') for x in p.relative_to(root).parts)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--config', choices=('Debug', 'Release'), default='Debug')
    parser.add_argument('--cmake', default='cmake', help='CMake executable, or put it in PATH')
    parser.add_argument('--gcc', default='arm-none-eabi-gcc', help='ARM GCC executable')
    parser.add_argument('--ninja', default='ninja', help='Ninja executable')
    parser.add_argument('--led', choices=('ON', 'OFF'), help='Override project LED default')
    parser.add_argument('--led-ms', type=int, help='LED toggle interval: 1..60000 ms')
    parser.add_argument('--continuous', choices=('ON', 'OFF'), help='Allow explicit continuous commands (default ON)')
    parser.add_argument('--startup-oneshot', choices=('ON', 'OFF'), help='Legacy automatic boot pulses (default OFF)')
    parser.add_argument('--jobs', type=int, default=6)
    parser.add_argument('--work-root', help='Optional existing ASCII directory for temporary builds')
    args = parser.parse_args()
    if args.led_ms is not None and not 1 <= args.led_ms <= 60000:
        parser.error('--led-ms must be between 1 and 60000')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    tools = {}
    for name in ('cmake', 'gcc', 'ninja'):
        value = shutil.which(getattr(args, name))
        if not value:
            parser.error(f'{name} was not found; supply --{name} or put it in PATH')
        tools[name] = str(Path(value).resolve())
    project = Path(__file__).resolve().parents[1]
    output = project/'build'/args.config
    output.mkdir(parents=True, exist_ok=True)
    before = manifest(project)
    work = Path(tempfile.mkdtemp(prefix='targetcarrier-stm32-', dir=args.work_root)).resolve()
    if not str(work).isascii():
        parser.error('Temporary path contains non-ASCII characters; use --work-root with an ASCII directory')
    source = work/'source'
    shutil.copytree(project, source, ignore=shutil.ignore_patterns('build', '.git', '__pycache__'))
    if manifest(source) != before:
        raise RuntimeError('Source changed while copying; no build was started')
    env = dict(os.environ)
    env['PATH'] = ';'.join(str(Path(tools[n]).parent) for n in ('gcc','ninja','cmake'))+';'+env['PATH']
    commands = [tools['cmake'], '-S', str(source), '-B', str(work/'build'), '-G', 'Ninja',
                '-DCMAKE_BUILD_TYPE='+args.config,
                '-DCMAKE_TOOLCHAIN_FILE='+str(source/'cmake/gcc-arm-none-eabi.cmake'),
                '-DCMAKE_MAKE_PROGRAM='+tools['ninja']]
    if args.led is not None:
        commands.append('-DTARGETCARRIER_LED_BLINK='+args.led)
    if args.led_ms is not None:
        commands.append('-DTARGETCARRIER_LED_TOGGLE_MS='+str(args.led_ms))
    if args.continuous is not None:
        commands.append('-DTARGETCARRIER_CONTINUOUS_ENABLED='+args.continuous)
    if args.startup_oneshot is not None:
        commands.append('-DTARGETCARRIER_STARTUP_ONESHOT='+args.startup_oneshot)
    for label, command in (('configure', commands), ('build', [tools['cmake'],'--build',str(work/'build'),'--parallel',str(args.jobs)])):
        run = subprocess.run(command, capture_output=True, env=env)
        text = (run.stdout+run.stderr).decode('utf-8', errors='replace')
        (output/(label+'.log')).write_text(text, encoding='utf-8')
        if run.returncode:
            raise RuntimeError(f'{label} failed (exit {run.returncode}); see {output/(label+".log")}\n{text}')
    if manifest(project) != before:
        raise RuntimeError('Source changed during build; artifacts were not copied back')
    artifacts = {}
    for suffix in ('elf','hex','bin','map'):
        p = work/'build'/('stmc8t6.'+suffix)
        shutil.copy2(p, output/p.name)
        artifacts[p.name] = digest(output/p.name)
    result = {'configuration':args.config, 'source_sha256':before, 'artifact_sha256':artifacts,
              'ascii_workdir':str(work), 'tool_paths':tools,
              'requested_continuous_override':args.continuous, 'requested_startup_oneshot_override':args.startup_oneshot,
              'requested_led_override':args.led, 'requested_led_ms_override':args.led_ms,
              'passed':True, 'hardware_tested':False}
    (output/'BUILD_RESULT.json').write_text(json.dumps(result, indent=2, ensure_ascii=False), encoding='utf-8')
    print(f'{args.config}: PASS; artifacts in {output}')
    log = (output/'build.log').read_text(encoding='utf-8')
    print('\n'.join(log.splitlines()[-5:]))
    print(f'ASCII sources retained for debugging: {work}')


if __name__ == '__main__':
    main()
