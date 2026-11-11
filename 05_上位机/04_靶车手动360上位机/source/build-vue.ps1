$ErrorActionPreference='Stop'
Set-Location -LiteralPath $PSScriptRoot
if(-not(Test-Path -LiteralPath '.venv')){python -m venv .venv}
& '.\.venv\Scripts\python.exe' -m pip install bleak==3.0.2 pywebview==6.1 pyinstaller==6.22.3
if($LASTEXITCODE){throw 'Dependency installation failed'}
& '.\.venv\Scripts\python.exe' -m unittest discover -s tests -v
if($LASTEXITCODE){throw 'Tests failed'}
& '.\.venv\Scripts\python.exe' -m PyInstaller --noconfirm --windowed --onedir --name TargetCarrier-PC-Vue --collect-all webview --collect-all pythonnet --collect-all clr_loader --collect-all bleak --collect-all winrt --hidden-import clr --add-data "$PSScriptRoot\web;web" desktop.py
if($LASTEXITCODE){throw 'Build failed'}
