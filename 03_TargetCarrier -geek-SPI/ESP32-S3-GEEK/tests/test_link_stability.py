"""Compatibility entry point for production radio regression tests."""
import argparse,subprocess,sys,tempfile
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('--master',type=Path,default=Path(__file__).resolve().parents[1]);p.add_argument('--slave',type=Path);p.add_argument('--output',type=Path);p.add_argument('--gcc',default=r'D:\mingw\mingw64\bin\gcc.exe');a=p.parse_args()
out=a.output or Path(tempfile.mkdtemp(prefix='dual-radio-'))
subprocess.run([sys.executable,str(a.master/'tests/test_dual_radio.py'),'--gcc',str(a.gcc),'--output',str(out)],check=True)
