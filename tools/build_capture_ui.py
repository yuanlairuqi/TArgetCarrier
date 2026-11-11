"""Build a dedicated capture console. Does not alter the preserved old executable."""
from pathlib import Path
import argparse,shutil,subprocess,sys,json
p=argparse.ArgumentParser();p.add_argument('--variant',choices=['UART','SPI','I2C'],required=True);p.add_argument('--out',type=Path,required=True);p.add_argument('--work',type=Path,required=True);a=p.parse_args()
root=Path(__file__).resolve().parents[1];name='TargetCarrier -统geek'+('' if a.variant=='UART' else '-'+a.variant);original=root/name/'上位机/TargetCarrier-Unified/source'
work=a.work.resolve();out=a.out.resolve()
if work.exists() or out.exists():p.error('Use fresh work and output paths')
source=work/'source';shutil.copytree(original,source,ignore=shutil.ignore_patterns('__pycache__','tests','.venv','*.local.json'));out.mkdir(parents=True)
cmd=[sys.executable,'-m','PyInstaller','--noconfirm','--windowed','--onedir','--name','TargetCarrier-Capture','--distpath',str(out),'--workpath',str(work/'build'),'--specpath',str(work),'--collect-all','webview','--collect-all','pythonnet','--collect-all','clr_loader','--collect-all','bleak','--collect-all','winrt','--hidden-import','clr','--hidden-import','serial.tools.list_ports_windows','--add-data',str(source/'web')+';web',str(source/'unified.py')]
with (work/'build.log').open('wb') as f:rc=subprocess.run(cmd,cwd=source,stdout=f,stderr=subprocess.STDOUT).returncode
(work/'result.json').write_text(json.dumps(dict(variant=a.variant,exit_code=rc,command=cmd),indent=2),encoding='utf8')
print(a.variant,'packaging',rc,flush=True);raise SystemExit(rc)
