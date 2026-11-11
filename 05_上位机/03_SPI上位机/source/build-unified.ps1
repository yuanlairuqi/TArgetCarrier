$ErrorActionPreference='Stop'
Set-Location -LiteralPath $PSScriptRoot
if(-not(Test-Path -LiteralPath '.venv\Scripts\python.exe')){python -m venv .venv}
& '.\.venv\Scripts\python.exe' -m pip install -r requirements-vue-build.txt
if($LASTEXITCODE){throw 'Dependency installation failed'}
& '.\.venv\Scripts\python.exe' -m unittest discover -s tests -v
if($LASTEXITCODE){throw 'Tests failed'}
$taskAssets=Join-Path $PSScriptRoot 'web'
& '.\.venv\Scripts\python.exe' -m PyInstaller --noconfirm --windowed --onedir --name TargetCarrier-Unified --collect-all webview --collect-all pythonnet --collect-all clr_loader --collect-all bleak --collect-all winrt --hidden-import clr --hidden-import serial.tools.list_ports_windows --add-data "$taskAssets;web" unified.py
if($LASTEXITCODE){throw 'Build failed'}
