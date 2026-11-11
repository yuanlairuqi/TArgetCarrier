"""Build only: self-contained module snapshot; no device or flashing access."""
import argparse, os, sys, json, shutil, subprocess, hashlib
from pathlib import Path

def sha(p): return hashlib.sha256(p.read_bytes()).hexdigest()

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--work', type=Path, required=True, help='Fresh ASCII build/output directory')
    p.add_argument('--target', choices=['all', 'esp', 'stm32', 'tests'], default='all')
    p.add_argument('--backend', choices=['all', '1', '2', '3'], default='all')
    p.add_argument('--role', choices=['all', '1', '2'], default='all')
    p.add_argument('--idf', type=Path, default=Path('C:/Espressif/v6.1/esp-idf'))
    p.add_argument('--tools', type=Path, default=Path('C:/Espressif/tools'))
    p.add_argument('--arm-bin', type=Path, help='Directory containing arm-none-eabi-gcc.exe')
    p.add_argument('--paired', action='store_true', help='Explicitly include real private pairing header')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    work = a.work.resolve()
    if work.exists() or not str(work).isascii() or work.is_relative_to(root):
        p.error('--work must be a fresh ASCII directory outside the source tree')
    if work.is_relative_to(Path('C:/Users/y/Desktop/TargetCarrier')):
        p.error('The preserved original project cannot be used as output')
    if a.paired and not (root/'config/tc_pairing.local.h').is_file():
        p.error('Real config/tc_pairing.local.h is required; no synthetic keys are generated')
    if a.target in ('all', 'stm32') and not a.arm_bin:
        p.error('--arm-bin is required for the reference STM32 build')
    work.mkdir(parents=True)
    skip = ['build', '__pycache__', '.git', 'sdkconfig', 'sdkconfig.old']
    if not a.paired: skip.append('tc_pairing.local.h')
    snapshot = work/'source'
    shutil.copytree(root, snapshot, ignore=shutil.ignore_patterns(*skip))
    manifest = {f.relative_to(snapshot).as_posix(): sha(f) for f in snapshot.rglob('*') if f.is_file()}
    (work/'source-sha256.json').write_text(json.dumps(manifest, indent=2), encoding='utf8')
    tools = a.tools.resolve()
    env = dict(os.environ, IDF_PATH=str(a.idf), IDF_TOOLS_PATH=str(tools),
        IDF_PYTHON_ENV_PATH=str(tools/'python/v6.1/venv'), ESP_IDF_VERSION='6.1',
        PROCESSOR_ARCHITECTURE='AMD64', PYTHONUTF8='1', IDF_CCACHE_ENABLE='0',
        IDF_PY_BUILD_JOBS='4', GIT_CONFIG_COUNT='1', GIT_CONFIG_KEY_0='safe.directory',
        GIT_CONFIG_VALUE_0=a.idf.as_posix())
    roms = list((tools/'esp-rom-elfs').glob('*'))
    if roms: env['ESP_ROM_ELF_DIR'] = str(roms[0])
    bins = ['cmake/4.0.3/bin', 'ninja/1.12.1',
        'xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin',
        'riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin',
        'python/v6.1/venv/Scripts']
    env['PATH'] = ';'.join(([str(a.arm_bin)] if a.arm_bin else []) + [str(tools/x) for x in bins]) + ';' + env['PATH']
    cmake = tools/'cmake/4.0.3/bin/cmake.exe'
    python = tools/'python/v6.1/venv/Scripts/python.exe'
    results = []
    def run(label, cmd):
        with (work/(label+'.log')).open('wb') as log:
            result = subprocess.run(list(map(str, cmd)), env=env, stdout=log, stderr=subprocess.STDOUT)
        results.append(dict(label=label, exit=result.returncode, command=list(map(str, cmd))))
        (work/'results.json').write_text(json.dumps(results, indent=2), encoding='utf8')
        print(label, result.returncode, flush=True)
        if result.returncode: raise RuntimeError(f'{label} failed; see {work/(label+".log")}')
    backends = (1,2,3) if a.backend == 'all' else (int(a.backend),)
    if a.target in ('all', 'stm32'):
        for backend in backends:
            b = work/f'stm32-{backend}'
            run(f'stm32-{backend}-configure', [cmake,'-S',snapshot/'firmware/stm32','-B',b,'-G','Ninja',
                '-DCMAKE_TOOLCHAIN_FILE='+str(snapshot/'firmware/stm32/toolchain.cmake'),
                '-DTC_BACKEND='+str(backend)])
            run(f'stm32-{backend}-build',[cmake,'--build',b,'--parallel','4'])
    if a.target in ('all','esp'):
        roles = (1,2) if a.role == 'all' else (int(a.role),)
        for role in roles:
            for backend in backends:
                b = work/'esp-build'
                run(f'esp32-{role}-{backend}', [python,a.idf/'tools/idf.py','-C',snapshot/'firmware/esp32','-B',b,
                    '-D','IDF_TARGET=esp32s3','-D',f'TC_ROLE={role}','-D',f'TC_BACKEND={backend}','build'])
                artifact = work/f'esp32-{role}-{backend}'
                artifact.mkdir()
                for name in ['tc_esp32.bin','tc_esp32.elf','tc_esp32.map','flasher_args.json']:
                    shutil.copy2(b/name,artifact/name)
                for folder in ['bootloader','partition_table']:
                    (artifact/folder).mkdir()
                    for f in (b/folder).glob('*.bin'): shutil.copy2(f,artifact/folder/f.name)
    if a.target in ('all','tests'):
        run('software-tests',[sys.executable,snapshot/'tests/run_tests.py',work/'host-tests'])
    after = {f.relative_to(snapshot).as_posix(): sha(f) for f in snapshot.rglob('*')
        if f.is_file() and f.name not in ('sdkconfig','sdkconfig.old') and '__pycache__' not in f.parts}
    # sdkconfig is the only intentional generated file in the source snapshot.
    if any(after.get(k) != v for k,v in manifest.items()): raise RuntimeError('Snapshot sources changed during build')
    print('Build complete. Hardware not accessed. Paired:', a.paired)

if __name__ == '__main__': main()
