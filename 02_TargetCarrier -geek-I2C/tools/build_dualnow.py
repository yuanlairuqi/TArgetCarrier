"""Build all three ESP projects in a fresh ASCII snapshot. Offline build only, never flash.
Usage: python tools/build_dualnow.py --out C:/.../fresh-ascii-path [--paired]
"""
from pathlib import Path
import argparse,os,shutil,subprocess,json,hashlib

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--out',type=Path,required=True);p.add_argument('--paired',action='store_true');p.add_argument('--idf',type=Path,default=Path(r'C:\Espressif\v6.1\esp-idf'));p.add_argument('--tools',type=Path,default=Path(r'C:\Espressif\tools'));a=p.parse_args()
 root=Path(__file__).resolve().parent.parent;out=a.out.resolve()
 if not str(out).isascii() or out.exists():p.error('Choose a fresh ASCII output directory')
 mapping={'wired_transport':'wired_transport','master':'ESP32-S3-GEEK','xslave':'x轴/esp_无线从机_20261022','target/bridge/ble_direct':'靶车/靶车/firmware/esp32_bridge/ble_direct','target/lib':'靶车/靶车/firmware/lib'}
 projects=['master','xslave','target/bridge/ble_direct']
 if a.paired and any(not (root/mapping[k]/'pairing.local.defaults').is_file() for k in projects):p.error('All 3 real pairing.local.defaults required')
 skip={'build','artifacts','verification','_backups','__pycache__','.git','.vscode','sdkconfig','sdkconfig.old','sdkconfig.paired'}
 if not a.paired:skip.add('pairing.local.defaults')
 out.mkdir(parents=True)
 for k,rel in mapping.items():shutil.copytree(root/rel,out/'src'/k,ignore=lambda d,n:[x for x in n if x in skip])
 env=dict(os.environ,IDF_PATH=str(a.idf),IDF_TOOLS_PATH=str(a.tools),IDF_PYTHON_ENV_PATH=str(a.tools/'python/v6.1/venv'),ESP_IDF_VERSION='6.1',PROCESSOR_ARCHITECTURE='AMD64',PYTHONUTF8='1',IDF_CCACHE_ENABLE='0',IDF_PY_BUILD_JOBS='4',GIT_CONFIG_COUNT='1',GIT_CONFIG_KEY_0='safe.directory',GIT_CONFIG_VALUE_0=a.idf.as_posix())
 bins=['cmake/4.0.3/bin','ninja/1.12.1','xtensa-esp-elf/esp-15.2.0_20251204/xtensa-esp-elf/bin','riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin','python/v6.1/venv/Scripts'];env['PATH']=';'.join(str(a.tools/x) for x in bins)+';'+env['PATH']
 results=[]
 for k in projects:
  src=out/'src'/k;b=out/'build'/k;b.mkdir(parents=True)
  defaults=str(src/'sdkconfig.defaults')+(';' +str(src/'pairing.local.defaults') if a.paired else '')
  cmd=[str(a.tools/'python/v6.1/venv/Scripts/python.exe'),str(a.idf/'tools/idf.py'),'-C',str(src),'-B',str(b),'-D','WIRED_TRANSPORT_ROOT='+str(out/'src/wired_transport'),'-D','IDF_TARGET=esp32s3','-D','SDKCONFIG='+str(b/'sdkconfig'),'-D','SDKCONFIG_DEFAULTS='+defaults,'build']
  with (b/'compile.log').open('wb') as f:rc=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
  results.append(dict(project=k,source=str(root/mapping[k]),paired=a.paired,exit_code=rc,command=cmd))
  (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf8')
  print(k,'build exit',rc)
  if rc:raise SystemExit(rc)
 manifest={str(f.relative_to(out/'src')):hashlib.sha256(f.read_bytes()).hexdigest() for f in (out/'src').rglob('*') if f.is_file() and f.name!='pairing.local.defaults'}
 (out/'source-sha256.json').write_text(json.dumps(manifest,ensure_ascii=False,indent=2),encoding='utf8')
 print('All ESP builds complete. No device accessed. Paired='+str(a.paired))
if __name__=='__main__':main()
