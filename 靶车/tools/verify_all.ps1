# ============================================================================
#  靶车 —— 一键验证脚本
#
#  做三件事:
#    1. C 端协议自测 (MSVC 编译并运行, 不需要硬件)
#    2. Python 端协议对拍 (独立实现, 互相印证帧格式)
#    3. 三个固件工程全部编译 (ESP32 主机 / M5Stamp 桥接 / STM32F103)
#
#  用法:
#    powershell -ExecutionPolicy Bypass -File tools\verify_all.ps1
#
#  注意: 脚本文件本身存为 UTF-8 with BOM, 否则 Windows PowerShell 5.1
#        会按 GBK 解读中文注释而报语法错误。
# ============================================================================

$ErrorActionPreference = 'Continue'

$repoRoot = Split-Path -Parent $PSScriptRoot
$pio = "$env:USERPROFILE\.platformio\penv\Scripts\pio.exe"
if (-not (Test-Path $pio)) {
    $pio = (Get-Command pio -ErrorAction SilentlyContinue).Source
}

$results = @()

function Record($name, $ok, $detail) {
    $script:results += [pscustomobject]@{ 项目 = $name; 结果 = $(if ($ok) { '通过' } else { '失败' }); 备注 = $detail }
}

# ---------------------------------------------------------------------------
Write-Host "`n===== 1/3  C 端协议自测 =====" -ForegroundColor Cyan
$out = & powershell.exe -NoProfile -ExecutionPolicy Bypass -File "$PSScriptRoot\run_protocol_test.ps1" 2>&1
$ok = ($LASTEXITCODE -eq 0)
Record '协议自测 (C/MSVC)' $ok $(if ($ok) { '全部断言通过' } else { '见上方输出' })

# ---------------------------------------------------------------------------
Write-Host "`n===== 2/3  Python 端协议对拍 =====" -ForegroundColor Cyan
$py = (Get-Command python -ErrorAction SilentlyContinue).Source
if ($py) {
    & $py "$PSScriptRoot\pc_monitor.py" --selftest | Out-Null
    $ok = ($LASTEXITCODE -eq 0)
    Record '协议对拍 (Python)' $ok $(if ($ok) { '与 C 端一致' } else { '两侧协议不一致!' })
} else {
    Record '协议对拍 (Python)' $false '找不到 python'
}

# ---------------------------------------------------------------------------
Write-Host "`n===== 3/3  固件编译 =====" -ForegroundColor Cyan
$projects = @(
    @{ dir = 'firmware\esp32_master'; env = 'esp32dev';         name = 'ESP32 主机 (esp32dev)' },
    @{ dir = 'firmware\esp32_master'; env = 'm5stack-stamps3';  name = 'ESP32 主机 (stamps3)' },
    @{ dir = 'firmware\esp32_bridge'; env = 'm5stack-stamps3';  name = '桥接端 (stamps3)' },
    @{ dir = 'firmware\stm32_f103';   env = 'bluepill_f103c8';  name = 'STM32F103' }
)

foreach ($p in $projects) {
    $full = Join-Path $repoRoot $p.dir
    Write-Host "`n--- $($p.name) ---" -ForegroundColor DarkGray
    Push-Location $full
    $log = & $pio run -e $p.env 2>&1
    Pop-Location

    $ok = [bool]($log | Select-String -Pattern 'SUCCESS')
    $size = ($log | Select-String -Pattern 'RAM:|Flash:' | ForEach-Object { $_.Line.Trim() }) -join ' | '
    $err  = ($log | Select-String -Pattern 'error:|Error 1|cannot open' | Select-Object -First 1)
    Record $p.name $ok $(if ($ok) { $size } else { "$err" })
}

# ---------------------------------------------------------------------------
Write-Host "`n================ 汇总 ================" -ForegroundColor Cyan
$results | Format-Table -AutoSize

$failed = @($results | Where-Object { $_.结果 -ne '通过' }).Count
if ($failed -eq 0) {
    Write-Host "全部通过。" -ForegroundColor Green
    exit 0
} else {
    Write-Host "$failed 项失败。" -ForegroundColor Red
    exit 1
}
