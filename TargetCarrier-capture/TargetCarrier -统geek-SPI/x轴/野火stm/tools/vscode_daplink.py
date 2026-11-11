"""VS Code build/flash entry point for the STM32F103C8 project."""
from pathlib import Path
import argparse
import datetime
import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, log=None):
    print(subprocess.list2cmdline([str(x) for x in command]), flush=True)
    result = subprocess.run([str(x) for x in command], stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    output = result.stdout.decode('utf-8', errors='replace')
    print(output, flush=True)
    if log:
        log.write_text(output, encoding='utf-8')
    if result.returncode:
        raise RuntimeError(f'Command failed: exit {result.returncode}')
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('check', 'build', 'probe', 'flash'))
    parser.add_argument('--config', choices=('Debug', 'Release'), default='Debug')
    args = parser.parse_args()
    project = Path(__file__).resolve().parents[1]
    config = json.loads((project/'.vscode/daplink-tools.json').read_text(encoding='utf-8-sig'))
    for name, value in config.items():
        if not Path(value).exists():
            raise FileNotFoundError(f'{name}: {value}')
    openocd = [config['openocd'], '-s', config['scripts'],
               '-f', 'interface/cmsis-dap.cfg', '-c', 'transport select swd',
               '-f', 'target/stm32f1x.cfg', '-c', 'adapter speed 1000',
               '-c', 'reset_config none',
               '-c', 'gdb_port disabled', '-c', 'tcl_port disabled',
               '-c', 'telnet_port disabled']
    if args.action == 'check':
        run([config['cmake'], '--version'])
        run([config['gcc'], '--version'])
        output = run(openocd + ['-c', 'adapter list', '-c', 'shutdown'])
        if 'cmsis-dap' not in output:
            raise RuntimeError('OpenOCD does not support CMSIS-DAP')
        print('PASS: tools and OpenOCD configuration; target was not connected.')
        return
    if args.action in ('build', 'flash'):
        run([sys.executable, project/'tools/build_stm32.py', '--config', args.config,
             '--cmake', config['cmake'], '--gcc', config['gcc'], '--ninja', config['ninja']])
    if args.action == 'build':
        return
    if args.action == 'probe':
        run(openocd + ['-c', 'init; targets; shutdown'])
        print('PASS: SWD connection only; no firmware written.')
        return
    output = project/'build'/args.config
    firmware = output/'stmc8t6.hex'
    build_result = json.loads((output/'BUILD_RESULT.json').read_text(encoding='utf-8'))
    if sha256(firmware) != build_result['artifact_sha256']['stmc8t6.hex']:
        raise RuntimeError('Firmware hash does not match the completed build')
    stamp = datetime.datetime.now().strftime('%Y%m%d-%H%M%S-%f')
    logs = project/'build/daplink'/stamp
    logs.mkdir(parents=True)
    # Use ASCII paths for Tcl/OpenOCD; retain the source project's Chinese path.
    work = Path(tempfile.mkdtemp(prefix='stm32-daplink-'))
    if not str(work).isascii():
        raise RuntimeError('Temporary directory must use an ASCII path')
    image = work/'firmware.hex'
    backup = work/'flash-before.bin'
    shutil.copy2(firmware, image)
    print('Backing up 64 KiB Flash before programming. Keep motor power disconnected.', flush=True)
    run(openocd + ['-c', f'init; reset halt; flash probe 0; dump_image {{{backup.as_posix()}}} 0x08000000 0x10000; shutdown'],
        logs/'backup.log')
    if not backup.exists() or backup.stat().st_size != 65536:
        raise RuntimeError('Backup incomplete; programming cancelled')
    shutil.copy2(backup, logs/backup.name)
    record = {'firmware': str(firmware), 'firmware_sha256': sha256(image),
              'backup_sha256': sha256(backup), 'flash_start': '0x08000000',
              'backup_bytes': 65536, 'verified': False}
    (logs/'RESULT.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    run(openocd + ['-c', f'program {{{image.as_posix()}}} verify reset exit'], logs/'flash.log')
    record['verified'] = True
    (logs/'RESULT.json').write_text(json.dumps(record, indent=2), encoding='utf-8')
    print(f'PASS: firmware written and verified. Backup and logs: {logs}')
    print('Motor operation and other physical behavior still require observation.')


if __name__ == '__main__':
    try:
        main()
    except Exception as exc:
        print(f'ERROR: {exc}', file=sys.stderr)
        sys.exit(1)
