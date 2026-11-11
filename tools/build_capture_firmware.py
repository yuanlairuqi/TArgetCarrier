"""Build the 7 distinct unpaired ESP capture configurations; never flash."""
from pathlib import Path
import shutil,json,subprocess,os
import argparse
p=argparse.ArgumentParser();p.add_argument("--variants",nargs="+",default=["uart","spi","i2c"]);p.add_argument("--result",default="firmware-results.json");p.add_argument("--work",type=Path,required=True);p.add_argument("--resume",action="store_true");a=p.parse_args()
R=Path(__file__).resolve().parents[1];W=a.work.resolve();
if W.exists() and not a.resume:p.error("Use a fresh ASCII work path or explicitly pass --resume")
if not str(W).isascii():p.error("Build path must be ASCII")
W.mkdir(parents=True,exist_ok=True)
tools=Path('C:/Espressif/tools');idf=Path('C:/Espressif/v6.1/esp-idf')
env=dict(os.environ,IDF_PATH=str(idf),IDF_TOOLS_PATH=str(tools),IDF_PYTHON_ENV_PATH=str(tools/'python/v6.1/venv'),ESP_ROM_ELF_DIR=str(tools/'esp-rom-elfs/20241011'),ESP_IDF_VERSION='6.1',PROCESSOR_ARCHITECTURE='AMD64',PYTHONUTF8='1',IDF_CCACHE_ENABLE='0',IDF_PY_BUILD_JOBS='4',GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='safe.directory',GIT_CONFIG_VALUE_0=idf.as_posix())
env['PATH']=';'.join(str(tools/p) for p in ['cmake/4.0.3/bin','ninja/1.12.1','xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin','riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin','python/v6.1/venv/Scripts'])+';'+env['PATH']
result_file=W/a.result
results=json.loads(result_file.read_text()) if result_file.exists() else []
for suffix,label in [('', 'uart'),('-SPI','spi'),('-I2C','i2c')]:
 if label not in a.variants:continue
 root=R/('TargetCarrier -统geek'+suffix);stage=W/(label+'_build');src=stage/'src'
 mappings=[('ESP32-S3-GEEK','master'),('x轴/esp_无线从机_20261022','xslave'),('靶车/靶车/firmware/esp32_bridge/ble_direct','target/bridge/ble_direct'),('靶车/靶车/firmware/lib','target/lib'),('wired_transport','wired_transport')]
 for origin,dest in mappings:
  if (root/origin).exists():shutil.copytree(root/origin,src/dest,dirs_exist_ok=True,ignore=shutil.ignore_patterns('build','build-*','artifacts','verification','__pycache__','.git','sdkconfig','sdkconfig.old','pairing.local.defaults'))
 for project in (['master'] if label=='uart' else [])+['xslave','target/bridge/ble_direct']:
  source=src/project;build=stage/'build'/project;build.mkdir(parents=True,exist_ok=True)
  cmd=[str(tools/'python/v6.1/venv/Scripts/python.exe'),str(idf/'tools/idf.py'),'-C',str(source),'-B',str(build),'-D','IDF_TARGET=esp32s3','-D','SDKCONFIG='+str(build/'sdkconfig'),'-D','SDKCONFIG_DEFAULTS='+str(source/'sdkconfig.defaults')]
  if suffix:cmd+=['-D','WIRED_TRANSPORT_ROOT='+str(src/'wired_transport')]
  cmd+=['build']
  with (build/'compile-final.log').open('wb') as f:rc=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
  results=[x for x in results if not(x['variant']==label and x['project']==project)]
  results.append(dict(variant=label,project=project,exit_code=rc,log=str(build/'compile-final.log'),paired=False))
  result_file.write_text(json.dumps(results,indent=2),encoding='utf8');print(label,project,rc,flush=True)
  if rc:raise SystemExit(rc)

