# ============================================================================
#  靶车 —— 一键验证脚本（CMake 版）
#
#  做三件事:
#    1. 用 CMake 编译三块板固件 + 协议自测程序
#    2. 跑 C 端协议自测 (ctest)
#    3. Python 端协议对拍 (独立实现, 互相印证帧格式)
#
#  用法:
#    pwsh -File tools\verify_all.ps1
#
#  等价的手工命令:
#    cmake --preset all
#    cmake --build --preset all
#    ctest --preset all
#    python tools\pc_monitor.py --selftest
#
#  注意: 脚本文件保存为 UTF-8 with BOM, 否则 Windows PowerShell 5.1
#        会按 GBK 解读中文注释而报语法错误。
# ============================================================================

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$results  = @()

function Record($name, $ok, $detail) {
    $script:results += [pscustomobject]@{ 项目 = $name; 结果 = $(if ($ok) { '通过' } else { '失败' }); 备注 = $detail }
}

# CMake / Ninja 装在用户目录, 新进程可能还没刷新 PATH, 这里补一下
$cmakeBin  = Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin'
$ninjaBin  = Join-Path $env:APPDATA 'Python\Python312\Scripts'
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

    # -----------------------------------------------------------------------
    Write-Host "`n===== 1/3  CMake 配置 + 编译三块板 =====" -ForegroundColor Cyan
    & $cmake --preset all 2>&1 | Out-Null
    if ($LASTEXITCODE -ne 0) {
        Record 'CMake 配置' $false 'cmake --preset all 失败'
    } else {
        Record 'CMake 配置' $true '预设 all'

        $log = & $cmake --build --preset all 2>&1
        $ok  = ($LASTEXITCODE -eq 0)
        $err = ($log | Select-String -Pattern 'error:|FAILED' | Select-Object -First 1)
        Record '三块板固件编译' $ok $(if ($ok) { 'ESP32 主机 / 桥接 / STM32 全部成功' } else { "$err" })

        foreach ($bin in @(
            'build\esp32_master\firmware.bin',
            'build\esp32_bridge\firmware.bin',
            'C:\cmake_build\target-vehicle\stm32\firmware.bin'
        )) {
            $full = if ([System.IO.Path]::IsPathRooted($bin)) { $bin } else { Join-Path $repoRoot $bin }
            $size = if (Test-Path $full) { '{0:N0} B' -f (Get-Item $full).Length } else { '缺失' }
            Record "产物 $(Split-Path -Leaf (Split-Path -Parent $full))" (Test-Path $full) $size
        }
    }

    # -----------------------------------------------------------------------
    Write-Host "`n===== 2/3  C 端协议自测 (ctest) =====" -ForegroundColor Cyan
    $log = & $ctest --preset all 2>&1
    $ok  = ($LASTEXITCODE -eq 0)
    Record '协议自测 (C)' $ok $(if ($ok) { '全部断言通过' } else { '见上方输出' })

    # -----------------------------------------------------------------------
    Write-Host "`n===== 3/3  Python 端协议对拍 =====" -ForegroundColor Cyan
    $py = (Get-Command python -ErrorAction SilentlyContinue).Source
    if ($py) {
        & $py (Join-Path $PSScriptRoot 'pc_monitor.py') --selftest | Out-Null
        $ok = ($LASTEXITCODE -eq 0)
        Record '协议对拍 (Python)' $ok $(if ($ok) { '与 C 端一致' } else { '两侧协议不一致!' })
    } else {
        Record '协议对拍 (Python)' $false '找不到 python'
    }

} finally {
    Pop-Location
}

# ---------------------------------------------------------------------------
Write-Host "`n================ 汇总 ================" -ForegroundColor Cyan
$results | Format-Table -AutoSize

$failed = @($results | Where-Object { $_.结果 -ne '通过' }).Count
if ($failed -eq 0) {
    Write-Host '全部通过。' -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failed 项失败。" -ForegroundColor Red
    exit 1
}
