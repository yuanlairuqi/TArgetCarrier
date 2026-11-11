from pathlib import Path
import os,sys,json,subprocess,shutil,hashlib

import argparse
parser=argparse.ArgumentParser(description='Build SPI/I2C business firmware, without device access')
parser.add_argument('--out',type=Path,required=True)
parser.add_argument('--target',choices=['all','stm','esp','master','ble','target-stm','target-esp'],default='all')
parser.add_argument('--sdk',type=Path,default=Path('C:/Users/y/.platformio/packages'))
parser.add_argument('--paired',action='store_true')
a=parser.parse_args();ROOT=Path(__file__).resolve().parents[1]
variant=json.loads((ROOT/'wired_transport/variant.json').read_text(encoding='utf8'))['variant'];kind=a.target
W=a.out.resolve();S=W/'src'
if W.exists() or not str(W).isascii() or W.is_relative_to(ROOT):parser.error('Choose a new ASCII output directory outside this project')
if W.is_relative_to(Path('C:/Users/y/Desktop/TargetCarrier -统geek')):parser.error('Cannot write build outputs to the preserved baseline')
if a.paired:
 for f in (['靶车/靶车/firmware/esp32_bridge/ble_direct'] if kind.startswith('target-') else ['ESP32-S3-GEEK','x轴/esp_无线从机_20261022','靶车/靶车/firmware/esp32_bridge/ble_direct']):
  if not (ROOT/f/'pairing.local.defaults').is_file():parser.error('Actual pairing.local.defaults files are required for the selected nodes')
W.mkdir(parents=True)
skip=shutil.ignore_patterns('build','build-*','verification','_backups','__pycache__','.git','.vscode','sdkconfig','sdkconfig.old',*(() if a.paired else ('pairing.local.defaults',)))
for origin,name in [('x轴/esp','xble'),('wired_transport','wired_transport'),('x轴/esp_无线从机_20261022','xesp'),('x轴/野火stm','xstm'),('靶车/靶车','vehicle'),('ESP32-S3-GEEK','master')]:
 if kind.startswith('target-') and name not in ('wired_transport','vehicle'):continue
 dest=S/name
 if not dest.exists():shutil.copytree(ROOT/origin,dest,ignore=skip)
# -- current implementation snapshots are never mixed with original-project files.
tools=Path('C:/Espressif/tools');idf=Path('C:/Espressif/v6.1/esp-idf');sdk=a.sdk.resolve();arm=sdk/'toolchain-gccarmnoneeabi/bin'
env=dict(os.environ,IDF_PATH=str(idf),IDF_TOOLS_PATH=str(tools),IDF_PYTHON_ENV_PATH=str(tools/'python/v6.1/venv'),ESP_IDF_VERSION='6.1',ESP_ROM_ELF_DIR=str(tools/'esp-rom-elfs/20241011'),PROCESSOR_ARCHITECTURE='AMD64',PYTHONUTF8='1',IDF_CCACHE_ENABLE='0',IDF_PY_BUILD_JOBS='4',GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='safe.directory',GIT_CONFIG_VALUE_0=idf.as_posix())
bins=['cmake/4.0.3/bin','ninja/1.12.1','xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin','riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin','python/v6.1/venv/Scripts'];env['PATH']=';'.join([str(arm)]+[str(tools/b) for b in bins])+';'+env['PATH']
cmake=tools/'cmake/4.0.3/bin/cmake.exe';py=tools/'python/v6.1/venv/Scripts/python.exe';results=[];resultfile=W/('results-'+kind+'.json')
def run(label,cmd):
 log=W/(label+'.log')
 with log.open('wb') as f:r=subprocess.run(list(map(str,cmd)),env=env,stdout=f,stderr=subprocess.STDOUT)
 results.append({'label':label,'exit':r.returncode,'command':list(map(str,cmd))});resultfile.write_text(json.dumps(results,indent=2),encoding='utf8');print(variant,label,r.returncode,flush=True)
 if r.returncode:raise RuntimeError(str(log))
wire='-DWIRED_TRANSPORT_ROOT='+str(S/'wired_transport')
if kind in ('stm','all','target-stm'):
 if kind!='target-stm':
  run('xstm-configure',[cmake,'-S',S/'xstm','-B',W/'xstm','-G','Ninja','-DCMAKE_TOOLCHAIN_FILE='+str(S/'xstm/cmake/gcc-arm-none-eabi.cmake'),'-DCMAKE_BUILD_TYPE=Release',wire])
  run('xstm',[cmake,'--build',W/'xstm','--parallel','4'])
 src=S/'vehicle/firmware/stm32_f103'
 run('target-stm-configure',[cmake,'-S',src,'-B',W/'target-stm','-G','Ninja','-DCMAKE_TOOLCHAIN_FILE='+str(S/'vehicle/cmake/toolchain-stm32f103.cmake'),'-DPIO_PACKAGES_DIR='+str(sdk),wire])
 run('target-stm',[cmake,'--build',W/'target-stm','--parallel','4'])
if kind in ('esp','all','master','ble','target-esp'):
 projects=[('xble',S/'xble')] if kind=='ble' else [('master',S/'master')] if kind=='master' else ([('master',S/'master')] if kind=='all' else [])+[('xble',S/'xble'),('xesp',S/'xesp'),('target-esp',S/'vehicle/firmware/esp32_bridge/ble_direct')]
 if kind=='target-esp':projects=[('target-esp',S/'vehicle/firmware/esp32_bridge/ble_direct')]
 for label,src in projects:
  b=W/label
  run(label,[py,idf/'tools/idf.py','-C',src,'-B',b,'-D','IDF_TARGET=esp32s3','-D','SDKCONFIG='+str(b/'sdkconfig'),'-D','SDKCONFIG_DEFAULTS='+str(src/'sdkconfig.defaults')+(';' +str(src/'pairing.local.defaults') if a.paired and (src/'pairing.local.defaults').is_file() else ''),'-D',wire[2:],'build'])
manifest={p.relative_to(S).as_posix():hashlib.sha256(p.read_bytes()).hexdigest() for p in S.rglob('*') if p.is_file() and p.name!='pairing.local.defaults'}
(W/'source-sha256.json').write_text(json.dumps(manifest,indent=2,ensure_ascii=False),encoding='utf8')
print(variant,kind,'complete; NO DEVICE ACCESSED',flush=True)
