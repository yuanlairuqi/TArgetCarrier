"""Run the X-axis firmware models and Android host regressions without hardware."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import tempfile


def main():
    root = Path(__file__).resolve().parents[1]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--gcc', default='gcc')
    parser.add_argument('--java-home', default=os.environ.get('JAVA_HOME', 'D:/uu'))
    parser.add_argument('--stm32', type=Path, default=root/'野火stm')
    parser.add_argument('--esp', type=Path, default=root/'esp')
    parser.add_argument('--android', type=Path, default=root/'android')
    parser.add_argument('--output', type=Path, default=root/'build/manual_control_tests')
    args = parser.parse_args()
    gcc = shutil.which(args.gcc)
    if not gcc:
        parser.error('Native GCC missing; supply --gcc (not arm-none-eabi-gcc)')
    java = Path(args.java_home)/'bin/java.exe'
    javac = Path(args.java_home)/'bin/javac.exe'
    work = Path(tempfile.mkdtemp(prefix='targetcarrier-tests-'))
    if not str(work).isascii():
        raise RuntimeError('Tests require an ASCII temporary directory')
    hashes = {}
    for name, source in (('stm', args.stm32), ('esp', args.esp), ('android', args.android)):
        for p in source.rglob('*'):
            rel = p.relative_to(source)
            if not p.is_file() or any(x in ('build', '.git', '.vscode', '.gradle', '__pycache__') for x in rel.parts):
                continue
            dest = work/name/rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(p, dest)
            hashes[name+'/'+rel.as_posix()] = hashlib.sha256(dest.read_bytes()).hexdigest()
    stm, esp, android = work/'stm', work/'esp', work/'android'
    output = work/'output'
    output.mkdir()
    env = dict(os.environ, PATH=str(Path(gcc).parent)+';'+os.environ['PATH'])
    passed, logs = [], []

    def run(command):
        result = subprocess.run([str(x) for x in command], capture_output=True, env=env)
        text = (result.stdout+result.stderr).decode('utf-8', errors='replace')
        logs.append(text)
        if result.returncode:
            raise RuntimeError(text)

    def build(name, sources, defines=(), includes=None):
        if includes is None:
            includes = [stm/x for x in ('tests/stubs', 'Core/Inc', 'App/Board', 'App/Motion', 'App/Link')]+[esp/'main']
        exe = output/(name+'.exe')
        run([gcc, '-std=c11', '-Wall', '-Wextra', '-Werror',
             *['-I'+str(p) for p in includes], *['-D'+d for d in defines], *sources, '-o', exe])
        return exe

    def test(name, exe, *arguments):
        run([exe, *arguments])
        passed.append(name)

    for enabled in (0, 1):
        test(f'continuous_{enabled}', build(f'continuous_{enabled}',
             [stm/'tests/test_continuous.c', stm/'App/Motion/motor_motion.c'],
             ['MOTOR_HOST_TEST', f'MOTOR_CONTINUOUS_ENABLED={enabled}']))
    sources = [stm/x for x in ('tests/test_robot_uart.c', 'App/Motion/motor_motion.c',
               'App/Link/robot_uart.c', 'App/Link/robot_wire.c', 'App/Link/robot_peer.c')]+[esp/'main/robot_bridge.c']
    uart = build('uart_disabled', sources, ['MOTOR_HOST_TEST', 'ROBOT_UART_HOST_TEST', 'MOTOR_CONTINUOUS_ENABLED=0'])
    for case in ('standalone', 'waiting-stop', 'running-stop', 'finished-batch-stop', 'rx-overflow',
                 'uart-error', 'rx-rearm-fail', 'bad-stream', 'new-session-move', 'tx-buffer',
                 'tx-timeout', 'tx-start-fail', 'link-timeout', 'duplicate', 'cross-bridge'):
        test('uart_'+case, uart, case)
    test('uart_manual_and_cycle', build('uart_enabled', sources,
         ['MOTOR_HOST_TEST', 'ROBOT_UART_HOST_TEST', 'MOTOR_CONTINUOUS_ENABLED=1']), 'enabled-cycle')
    for startup in (0, 1):
        test(f'rtos_boot_oneshot_{startup}', build(f'boot_{startup}',
             [stm/'tests/rtos_boot/test_app.c', stm/'App/RTOS/app_rtos.c'],
             ['APP_LED_BLINK_ENABLED=0', f'MOTOR_STARTUP_ONESHOT_ENABLED={startup}'],
             [stm/'tests/rtos_boot', stm/'App/RTOS', stm/'App/Motion']))

    classes = output/'java'
    classes.mkdir()
    source = android/'app/src/main/java/cn/robot/ble'
    names = ('Protocol', 'CommandChannel', 'HardwareState', 'ReceiveHistory', 'PulseHistory')
    tests = ('CommandChannelTest', 'HardwareStateTest', 'ProtocolTest', 'ReceiveHistoryTest', 'PulseHistoryTest')
    run([javac, '-encoding', 'UTF-8', '-d', classes,
         *[source/(n+'.java') for n in names], *[android/'tests'/(n+'.java') for n in tests]])
    for name in tests:
        fixture = {'ProtocolTest': 'status.bin', 'PulseHistoryTest': 'pulse.bin'}.get(name)
        test('android_'+name, java, '-ea', '-cp', classes, name,
             *([android/'tests/fixtures'/fixture] if fixture else []))
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output/'RESULT.json').write_text(json.dumps({
        'passed': len(passed), 'groups': passed, 'source_sha256': hashes,
        'snapshot': str(work), 'hardware_tested': False}, ensure_ascii=False, indent=2), encoding='utf-8')
    (args.output/'tests.log').write_text('\n'.join(logs), encoding='utf-8')
    print(f'PASS: {len(passed)} regression groups; {args.output}')


if __name__ == '__main__':
    main()
