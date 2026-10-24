param([string]$Gcc='D:\mingw\mingw64\bin\gcc.exe')
$ErrorActionPreference='Stop'
$env:PATH=(Split-Path $Gcc)+';'+$env:PATH
$master=Split-Path $PSScriptRoot
$repo=Split-Path $master
$slave=Join-Path $repo 'x轴\esp_无线从机_20261022'
$out=Join-Path $PSScriptRoot 'test-output'
New-Item -ItemType Directory -Force -Path $out | Out-Null
& $Gcc -std=c11 -Wall -Wextra -Werror -I "$slave\main" -I "$master\main" "$PSScriptRoot\test_firmware.c" "$slave\main\now_protocol.c" "$slave\main\robot_control.c" "$slave\main\pulse_wire.c" "$master\main\usb_wire.c" -o "$out\test_firmware.exe"
if($LASTEXITCODE){throw 'Host compile failed'}
& "$out\test_firmware.exe"
if($LASTEXITCODE){throw 'Host tests failed'}
& $Gcc -std=c11 -Wall -Wextra -Werror -I "$PSScriptRoot\radio_stubs" -I "$slave\main" "$PSScriptRoot\test_radio_queue.c" "$slave\main\now_protocol.c" "$slave\main\robot_control.c" -o "$out\test_radio_queue.exe"
if($LASTEXITCODE){throw 'Radio queue host compile failed'}
& "$out\test_radio_queue.exe"
if($LASTEXITCODE){throw 'Radio queue tests failed'}
