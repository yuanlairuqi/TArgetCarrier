$ErrorActionPreference='Stop'
Set-Location -LiteralPath $PSScriptRoot
$parent = Split-Path -Parent $PSScriptRoot
$distDir = Join-Path $parent 'app-diagnosis-20261107'
$workDir = Join-Path $PSScriptRoot 'build-diagnosis'
$taskAssets = Join-Path $PSScriptRoot 'web'
$python = Join-Path $PSScriptRoot '.venv\Scripts\python.exe'
if (-not (Test-Path -LiteralPath $python)) { $python = (Get-Command python).Source }
$deps = Join-Path $PSScriptRoot '.builddeps'
if (Test-Path -LiteralPath $deps) { $env:PYTHONPATH = $deps }
& $python -m unittest discover -s tests -v
if ($LASTEXITCODE) { throw 'Tests failed' }
& $python -m PyInstaller --noconfirm --clean --windowed --onedir --name TargetCarrier-Capture `
    --distpath "$distDir" --workpath "$workDir" --specpath "$workDir" `
    --collect-all webview --collect-all pythonnet --collect-all clr_loader `
    --collect-all bleak --collect-all winrt --hidden-import clr `
    --hidden-import serial.tools.list_ports_windows --add-data "$taskAssets;web" unified.py
if ($LASTEXITCODE) { throw 'Build failed' }
Write-Host "Built: $distDir\TargetCarrier-Capture\TargetCarrier-Capture.exe"
