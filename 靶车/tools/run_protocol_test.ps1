# ============================================================================
#  靶车 —— 协议库主机端自测（CMake 版）
#
#  在电脑上直接编译并运行 firmware/lib/protocol/test/test_protocol.c，
#  不需要插任何硬件。改了 protocol.h 之后务必先跑这个，再烧板子。
#
#  用法:
#     pwsh -File tools\run_protocol_test.ps1
#
#  这个脚本现在只是 CMake 的一层薄封装。等价的纯 CMake 命令:
#     cmake --preset all
#     cmake --build --preset all --target test_protocol
#     ctest --preset all
#
#  注意: 脚本文件保存为 UTF-8 with BOM，否则 Windows PowerShell 5.1
#        会按 GBK 解读中文注释而报语法错误。
# ============================================================================

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot

# CMake / Ninja 装在用户目录，新进程可能还没刷新 PATH，这里补一下
$cmakeBin = Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin'
$ninjaBin = Join-Path $env:APPDATA 'Python\Python312\Scripts'
if (Test-Path $cmakeBin) { $env:PATH = "$cmakeBin;$env:PATH" }
if (Test-Path $ninjaBin) { $env:PATH = "$ninjaBin;$env:PATH" }

$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) {
    Write-Host '找不到 cmake。请先按 docs\CMake构建说明.md 第二章安装。' -ForegroundColor Red
    exit 2
}
$ctest = Join-Path (Split-Path -Parent $cmake) 'ctest.exe'
if (-not (Test-Path $ctest)) { $ctest = 'ctest' }

Push-Location $repoRoot
try {
    Write-Host '配置 + 编译 test_protocol ...' -ForegroundColor DarkGray
    & $cmake --preset all | Out-Null
    & $cmake --build --preset all --target test_protocol
    if ($LASTEXITCODE -ne 0) {
        Write-Host "编译失败 (exit $LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }

    Write-Host "`n运行协议自测 ..." -ForegroundColor DarkGray
    & $ctest --preset all --output-on-failure
    $rc = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($rc -ne 0) {
    Write-Host "协议自测失败 (exit $rc)" -ForegroundColor Red
    exit $rc
}
Write-Host '协议自测通过。' -ForegroundColor Green
exit 0
