"""Run portable host checks in a fresh output directory; no device access."""
from pathlib import Path
import subprocess,os,argparse,sys
p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--gcc',type=Path,default=Path('D:/mingw/code/mingw64/bin/gcc.exe'));a=p.parse_args();out=a.out.resolve()
if out.exists():p.error('Choose a fresh output directory')
out.mkdir(parents=True);r=Path(__file__).resolve().parents[1]/'recorder_core';env=dict(os.environ);env['PATH']=str(a.gcc.parent)+';'+env['PATH']
cases=[('capture',['capture.c','test_capture.c'],[]),('recorder',['capture.c','recorder.c','test_recorder.c'],[]),('batch',['batch.c','test_batch.c'],[]),('fragment',['batch.c','fragment.c','test_fragment.c'],[]),('pipeline',['capture.c','recorder.c','batch.c','fragment.c','test_pipeline.c'],[]),('source',['capture.c','fragment.c','source.c','test_source.c'],[]),('storage',['tests/test_storage.c'],['-I'+str(r/'tests/mocks')]),('sd-adapter',['tests/test_sd_adapter.c'],['-I'+str(r/'tests/mocks')]),('boundaries',['tests/test_boundaries.c'],['-I'+str(r/'tests/mocks')]),('e2e',['capture.c','fragment.c','source.c','tests/test_e2e.c'],['-I'+str(r/'tests/mocks')])]
for name,files,flags in cases:
 exe=out/(name+'.exe');cmd=[str(a.gcc),'-std=c11','-Wall','-Wextra','-Wno-misleading-indentation',*flags,*[str(r/f) for f in files],'-o',str(exe)]
 with (out/(name+'.log')).open('wb') as f:
  rc=subprocess.run(cmd,env=env,stdout=f,stderr=subprocess.STDOUT).returncode
  if not rc:rc=subprocess.run([str(exe)],env=env,cwd=out,stdout=f,stderr=subprocess.STDOUT).returncode
 if rc:raise RuntimeError(str(out/(name+'.log')))
 print(name,'PASS',flush=True)
with (out/'offline.log').open('wb') as f:
 rc=subprocess.run([sys.executable,'-m','unittest','test_offline_history.py'],cwd=r,stdout=f,stderr=subprocess.STDOUT).returncode
if rc:raise RuntimeError(str(out/'offline.log'))
print('offline history PASS')

sys.path.insert(0,str(r.parent/'TargetCarrier -统geek/上位机/TargetCarrier-Unified/source'))
from wave_history import rows,export_csv
files=list((out/'e2e_sd/tcwave').glob('*.twl'));assert len(files)==1
records=list(rows(files[0]));assert [x['value'] for x in records]==[123,3300,9]
assert [x['seq'] for x in records]==[1,2,3]
assert all(x['node']==1 and x['boot']==7 and x['us']==2100000 for x in records)
assert [x['value'] for x in rows(files[0],node=2)]==[]
export_csv(files[0],out/'e2e.csv');print('C -> raw file -> Python replay/CSV PASS')
