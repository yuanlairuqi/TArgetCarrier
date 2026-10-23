"""Build a fresh ASCII-path snapshot with the installed ESP-IDF; never flash.
Usage: python build_snapshot.py --source <project> --out C:\\tc-build\\new-run [--paired]
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
from pathlib import Path
def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--source',type=Path,default=Path(__file__).resolve().parent)
    p.add_argument('--out',type=Path,required=True);p.add_argument('--paired',action='store_true')
    p.add_argument('--idf',type=Path,default=Path(r'C:\Espressif\v6.1\esp-idf'))
    p.add_argument('--tools',type=Path,default=Path(r'C:\Espressif\tools'))
    a=p.parse_args();source=a.source.resolve();out=a.out.resolve()
    if not str(out).isascii():p.error('--out must be an ASCII path for the Windows build tools')
    if out.exists():p.error('--out already exists; choose a fresh output directory')
    if not (source/'CMakeLists.txt').exists():p.error('source has no CMakeLists.txt')
    if a.paired and not (source/'pairing.local.defaults').exists():p.error('pairing.local.defaults missing; run configure_pairing.py using real STA MACs')
    skip={'build','artifacts','__pycache__','.git','.vscode','sdkconfig','sdkconfig.old','sdkconfig.paired'}
    out.mkdir(parents=True);snapshot=out/'src'
    shutil.copytree(source,snapshot,ignore=lambda d,n:[x for x in n if x in skip])
    manifest={str(f.relative_to(snapshot)):hashlib.sha256(f.read_bytes()).hexdigest() for f in snapshot.rglob('*') if f.is_file()}
    (out/'source-sha256.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
    env=dict(os.environ,IDF_PATH=str(a.idf),IDF_TOOLS_PATH=str(a.tools),IDF_PYTHON_ENV_PATH=str(a.tools/'python/v6.1/venv'),
        ESP_IDF_VERSION='6.1',PROCESSOR_ARCHITECTURE='AMD64',PYTHONUTF8='1',IDF_CCACHE_ENABLE='0',
        GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='safe.directory',GIT_CONFIG_VALUE_0=a.idf.as_posix())
    bins=['cmake/4.0.3/bin','ninja/1.12.1','xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin',
        'riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin','python/v6.1/venv/Scripts']
    env['PATH']=';'.join(str(a.tools/b) for b in bins)+';'+env['PATH']
    command=[str(a.tools/'python/v6.1/venv/Scripts/python.exe'),str(a.idf/'tools/idf.py'),'-C',str(snapshot),'-D','IDF_TARGET=esp32s3']
    if a.paired:command+=['-D','SDKCONFIG_DEFAULTS=sdkconfig.defaults;pairing.local.defaults']
    command+=['build']
    with (out/'build.log').open('wb') as log:
        result=subprocess.run(command,env=env,stdout=log,stderr=subprocess.STDOUT)
    (out/'result.json').write_text(json.dumps(dict(command=command,exit_code=result.returncode,source=str(source),snapshot=str(snapshot)),ensure_ascii=False,indent=2),encoding='utf8')
    print(f'Build exit={result.returncode}; log={out / "build.log"}; NO DEVICE ACCESSED')
    raise SystemExit(result.returncode)
if __name__=='__main__':main()
