import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

class PulsePacingTest(unittest.TestCase):
    def test_failure_aborts_and_ble_packets_are_paced(self):
        tests=Path(__file__).resolve().parent
        gcc=shutil.which("gcc") or r"D:\mingw\code\mingw64\bin\gcc.exe"
        env=os.environ.copy()
        env["PATH"]=str(Path(gcc).parent)+os.pathsep+env.get("PATH", "")
        with tempfile.TemporaryDirectory() as tmp:
            exe=Path(tmp)/("pulse_pacing.exe" if os.name=="nt" else "pulse_pacing")
            subprocess.run([gcc,"-std=c11","-I",str(tests/"pulse_pacing_mocks"),"-o",str(exe),str(tests/"test_pulse_pacing.c"),str(tests.parent/"main"/"pulse_wire.c")],check=True,env=env)
            subprocess.run([str(exe)],check=True,env=env)

if __name__ == "__main__": unittest.main()
