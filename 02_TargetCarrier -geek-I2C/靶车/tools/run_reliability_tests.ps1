# ============================================================================
#  靶车 —— 通信可靠性测试一键脚本（C 主机端自测 + Python 协议对拍）
#
#  这个脚本按契约 §9 的顺序做两件事：
#    1) 如果本机有 CMake：配置并编译四个**不接硬件**的主机端测试
#         test_protocol    （帧格式/CRC/编解码）
#         test_ws_proto    （WebSocket SHA-1/Base64/握手/帧）
#         test_linkcore    （序号/过期/ARM/新鲜度/统计/CAN 协调/成帧）
#       然后 ctest 跑一遍；
#    2) 无论有没有 CMake，都运行 Python 独立实现对拍：
#         python tools\test_protocol_v11.py
#
#  找不到 CMake 时不会中断 Python 测试，只打印清晰提示（C 端测试记为"跳过"）。
#
#  用法:
#     pwsh -File tools\run_reliability_tests.ps1
#     pwsh -File tools\run_reliability_tests.ps1 -BuildDir D:\tc-test-build
#     pwsh -File tools\run_reliability_tests.ps1 -NoClean      # 复用已有构建目录
#     pwsh -File tools\run_reliability_tests.ps1 -SkipC        # 只跑 Python 对拍
#
#  退出码: 0 = 所有已执行的测试通过；1 = 有测试失败。
#  （CMake 缺失只跳过 C 端，不算失败；Python 测试失败才算失败。）
#
#  ★ 注意: 本机可能没有 ESP32/STM32 SDK，但这里只编主机端测试，不需要交叉工具链。
#    四个固件的真实编译/上板验证见 docs\通信可靠性与FreeRTOS移植说明.md。
#
#  ★ 脚本文件保存为 UTF-8 with BOM，否则 Windows PowerShell 5.1 会按 GBK
#    解读中文注释而报语法错误（与 tools\*.ps1 的既有约定一致）。
# ============================================================================

[CmdletBinding()]
param(
    # 主机端测试的 CMake 构建目录。默认放系统临时目录，避免污染工程树。
    [string]$BuildDir = '',
    # 复用已有构建目录（不删除重来）
    [switch]$NoClean,
    # 只跑 Python 协议对拍（受限环境/没有 CMake 时也能一键验证 Python 部分）
    [switch]$SkipC,
    # cmake 配置/ctest 的超时秒数（受限环境里 CMake 的编译器探测可能卡住，超时后继续跑 Python）
    [int]$CMakeTimeoutSec = 180,
    # 编译四个测试目标的超时秒数
    [int]$BuildTimeoutSec = 600
)

$ErrorActionPreference = 'Stop'

$script:RepoRoot = Split-Path -Parent $PSScriptRoot
$script:Failures = New-Object System.Collections.Generic.List[string]

function Write-Step([string]$msg) {
    Write-Host ''
    Write-Host ('=== ' + $msg + ' ===') -ForegroundColor Cyan
}

function Add-Failure([string]$msg) {
    $script:Failures.Add($msg)
    Write-Host ('  [FAIL] ' + $msg) -ForegroundColor Red
}

function Write-Skip([string]$msg) {
    Write-Host ('  [SKIP] ' + $msg) -ForegroundColor Yellow
}

# 运行外部工具并带超时。
# 为什么不用 `& exe args` 直接跑：某些受限环境/杀软会让 CMake 的编译器探测
# （try_compile 执行子进程并捕获输出）永久卡住；这里用 Start-Process + 超时兜底，
# 超时后杀掉进程并明确报错，保证后面的 Python 测试一定跑得到。
function Invoke-ExternalTool {
    param(
        [Parameter(Mandatory = $true)][string]$Exe,
        [string[]]$ArgList = @(),
        [int]$TimeoutSec = 180,
        [string]$WorkingDirectory = ''
    )
    $id = [guid]::NewGuid().ToString('N')
    $outFile = Join-Path ([IO.Path]::GetTempPath()) ('targetcarrier_' + $id + '.out.log')
    $errFile = Join-Path ([IO.Path]::GetTempPath()) ('targetcarrier_' + $id + '.err.log')
    $quoted = @($ArgList | ForEach-Object {
        if ($_ -match '\s') { '"' + $_ + '"' } else { $_ }
    })
    $sp = @{
        FilePath               = $Exe
        ArgumentList           = $quoted
        NoNewWindow            = $true
        PassThru               = $true
        RedirectStandardOutput = $outFile
        RedirectStandardError  = $errFile
    }
    if ($WorkingDirectory) { $sp['WorkingDirectory'] = $WorkingDirectory }
    $proc = Start-Process @sp
    $timedOut = $false
    if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
        $timedOut = $true
        try { $proc.Kill() } catch { }
    }
    return [pscustomobject]@{
        ExitCode = $(if ($timedOut) { -1 } else { $proc.ExitCode })
        TimedOut = $timedOut
        OutFile  = $outFile
        ErrFile  = $errFile
    }
}

function Show-ToolTail([pscustomobject]$Result, [int]$Lines = 15) {
    foreach ($f in @($Result.ErrFile, $Result.OutFile)) {
        if ($f -and (Test-Path $f)) {
            Get-Content $f -Tail $Lines -ErrorAction SilentlyContinue |
                Where-Object { $_ -match '\S' } |
                ForEach-Object { Write-Host ('    ' + $_) -ForegroundColor DarkGray }
        }
    }
}

function Remove-ToolLogs([pscustomobject]$Result) {
    foreach ($f in @($Result.OutFile, $Result.ErrFile)) {
        if ($f -and (Test-Path $f)) { Remove-Item $f -Force -ErrorAction SilentlyContinue }
    }
}

# ---------------------------------------------------------------------------
# 定位工具链
# ---------------------------------------------------------------------------

function Find-CMake {
    # 1) PATH 里已有
    $cmd = Get-Command cmake -CommandType Application -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }

    $candidates = New-Object System.Collections.Generic.List[string]

    # 2) 用户级安装（VS Code CMake Tools 常见位置）
    $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe'))
    # 3) Espressif 工具链自带（本机 ESP-IDF 环境常见位置）
    $espressifRoot = 'C:\Espressif\tools\cmake'
    if (Test-Path $espressifRoot) {
        Get-ChildItem $espressifRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates.Add((Join-Path $_.FullName 'bin\cmake.exe')) }
    }
    # 4) 常见系统安装位置（兜底，不影响上面的优先顺序）
    $candidates.Add('C:\Program Files\CMake\bin\cmake.exe')
    $candidates.Add('C:\Program Files (x86)\CMake\bin\cmake.exe')
    $candidates.Add((Join-Path $env:APPDATA 'Python\Python312\Scripts\cmake.exe'))

    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Find-CTest([string]$cmakeExe) {
    $sameDir = Join-Path (Split-Path -Parent $cmakeExe) 'ctest.exe'
    if (Test-Path $sameDir) { return $sameDir }
    $cmd = Get-Command ctest -CommandType Application -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    return $null
}

function Find-Ninja {
    $cmd = Get-Command ninja -CommandType Application -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $candidates = New-Object System.Collections.Generic.List[string]
    $candidates.Add((Join-Path $env:APPDATA 'Python\Python312\Scripts\ninja.exe'))
    $candidates.Add((Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\ninja.exe'))
    # Espressif 工具链自带 ninja（本机 CMake 也来自这里，通常成对存在）
    $espressifRoot = 'C:\Espressif\tools\ninja'
    if (Test-Path $espressifRoot) {
        Get-ChildItem $espressifRoot -Directory -ErrorAction SilentlyContinue |
            Sort-Object Name -Descending |
            ForEach-Object { $candidates.Add((Join-Path $_.FullName 'ninja.exe')) }
    }
    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Find-CC {
    # 主机端测试只需要一个本机 C 编译器（不是 ESP32/STM32 交叉工具链）
    foreach ($name in @('gcc', 'clang', 'cc')) {
        $cmd = Get-Command $name -CommandType Application -ErrorAction SilentlyContinue
        if ($cmd) { return $cmd.Source }
    }
    $candidates = @(
        'C:\mingw64\bin\gcc.exe',
        'D:\mingw\mingw64\bin\gcc.exe',
        'C:\msys64\mingw64\bin\gcc.exe',
        'C:\Program Files\mingw-w64\mingw64\bin\gcc.exe'
    )
    if (Test-Path 'C:\Program Files\mingw-w64') {
        Get-ChildItem 'C:\Program Files\mingw-w64' -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { $candidates += (Join-Path $_.FullName 'mingw64\bin\gcc.exe') }
    }
    foreach ($p in $candidates) {
        if ($p -and (Test-Path $p)) { return (Resolve-Path $p).Path }
    }
    return $null
}

function Find-Python {
    # 任务约定：优先用 Espressif 自带 Python（本机没有 pip/系统 Python 时的可靠选择）
    $espressif = 'C:\Espressif\tools\python\python.exe'
    if (Test-Path $espressif) { return $espressif }
    $cmd = Get-Command python -CommandType Application -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $py = Get-Command py -CommandType Application -ErrorAction SilentlyContinue
    if ($py) { return $py.Source }   # 可能是 py 启动器，下面用 -3
    return $null
}

# ---------------------------------------------------------------------------
# 1) C 端：cmake 配置 + 编译四个测试 + ctest
# ---------------------------------------------------------------------------

$cmake = Find-CMake
$cBuildDir = $BuildDir
if (-not $cBuildDir) {
    $cBuildDir = Join-Path ([IO.Path]::GetTempPath()) 'targetcarrier_reliability_build'
}

$cExecuted = $false
$ctestExe = $null

if ($SkipC) {
    Write-Skip '按 -SkipC 跳过 C 端测试（只跑 Python 协议对拍）'
} elseif (-not $cmake) {
    Write-Skip '未找到 cmake，跳过 C 端 test_protocol / test_ws_proto / test_linkcore / test_can_driver'
    Write-Host ''
    Write-Host '  如何补齐 CMake（任选其一）：' -ForegroundColor Yellow
    Write-Host '    1) 安装 CMake 3.25+ 并勾选 "Add CMake to the system PATH"；'
    Write-Host '    2) 若使用 ESP-IDF：$env:PATH 加入 C:\Espressif\tools\cmake\<版本>\bin；'
    Write-Host '    3) 参考 docs\CMake构建说明.md 第二章，或先跑 tools\setup_cmake_env.ps1 体检。'
    Write-Host '  本次仍会继续执行 Python 协议对拍测试。' -ForegroundColor Yellow
} else {
    Write-Step ('C 端主机测试（cmake: ' + $cmake + '）')
    $ctestExe = Find-CTest $cmake
    if (-not $ctestExe) {
        Add-Failure '找到 cmake 但找不到配套的 ctest，无法执行 ctest'
    } else {
        # 构建目录默认在系统临时目录；删除前确认确实在临时目录内，避免误删
        $fullBuild = [IO.Path]::GetFullPath($cBuildDir)
        $tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
        if ((-not $NoClean) -and (Test-Path $fullBuild)) {
            if ($fullBuild.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) {
                Write-Host ('  清理旧构建目录: ' + $fullBuild) -ForegroundColor DarkGray
                Remove-Item -LiteralPath $fullBuild -Recurse -Force
            } else {
                Write-Host ('  构建目录不在系统临时目录，安全起见不自动删除: ' + $fullBuild) -ForegroundColor DarkGray
            }
        }

        $configureArgs = @('-S', $script:RepoRoot, '-B', $fullBuild, '-DCMAKE_BUILD_TYPE=Debug')
        $ninja = Find-Ninja
        if ($ninja) {
            $configureArgs += @('-G', 'Ninja')
            $env:PATH = (Split-Path -Parent $ninja) + ';' + $env:PATH
            Write-Host ('  生成器: Ninja (' + $ninja + ')') -ForegroundColor DarkGray
        } else {
            Write-Host '  未找到 ninja，使用 CMake 默认生成器（可能需要本机 C 编译器）' -ForegroundColor DarkGray
        }
        $cc = Find-CC
        if ($cc) {
            $configureArgs += @('-DCMAKE_C_COMPILER=' + $cc)
            $env:PATH = (Split-Path -Parent $cc) + ';' + $env:PATH
            Write-Host ('  C 编译器: ' + $cc) -ForegroundColor DarkGray
        } else {
            Write-Host '  未找到 gcc/clang/cc；若配置失败请安装 MinGW 或设置 $env:CC' -ForegroundColor Yellow
        }

        try {
            Write-Host '  [1/3] 配置顶层工程（只编译主机端测试，不碰交叉固件）...' -ForegroundColor DarkGray
            $r = Invoke-ExternalTool -Exe $cmake -ArgList $configureArgs -TimeoutSec $CMakeTimeoutSec
            if ($r.TimedOut) {
                Show-ToolTail $r
                throw ('cmake 配置超时（>' + $CMakeTimeoutSec + 's），已终止。日志: ' + $r.OutFile)
            }
            if ($r.ExitCode -ne 0) {
                Show-ToolTail $r
                throw ('cmake 配置失败，退出码 ' + $r.ExitCode)
            }
            Remove-ToolLogs $r

            Write-Host '  [2/3] 编译 test_protocol / test_ws_proto / test_linkcore / test_can_driver ...' -ForegroundColor DarkGray
            $buildArgs = @('--build', $fullBuild, '--target', 'test_protocol', 'test_ws_proto', 'test_linkcore', 'test_can_driver')
            $r = Invoke-ExternalTool -Exe $cmake -ArgList $buildArgs -TimeoutSec $BuildTimeoutSec
            if ($r.TimedOut) {
                Show-ToolTail $r
                throw ('编译测试目标超时（>' + $BuildTimeoutSec + 's），已终止。日志: ' + $r.OutFile)
            }
            if ($r.ExitCode -ne 0) {
                Show-ToolTail $r
                throw ('编译测试目标失败，退出码 ' + $r.ExitCode)
            }
            Remove-ToolLogs $r

            Write-Host '  [3/3] ctest ...' -ForegroundColor DarkGray
            $r = Invoke-ExternalTool -Exe $ctestExe -ArgList @('--output-on-failure') -TimeoutSec $CMakeTimeoutSec -WorkingDirectory $fullBuild
            if ($r.TimedOut) {
                Show-ToolTail $r
                throw ('ctest 超时（>' + $CMakeTimeoutSec + 's），已终止。日志: ' + $r.OutFile)
            }
            if ($r.ExitCode -ne 0) {
                Show-ToolTail $r
                throw ('ctest 有失败用例，退出码 ' + $r.ExitCode)
            }
            Remove-ToolLogs $r
            $cExecuted = $true
            Write-Host '  C 端测试全部通过 [OK]' -ForegroundColor Green
        } catch {
            Add-Failure ('C 端测试未通过: ' + $_.Exception.Message)
        }
    }
}

# ---------------------------------------------------------------------------
# 2) Python 独立实现协议对拍（无论有没有 CMake 都跑）
# ---------------------------------------------------------------------------

Write-Step 'Python 协议 v1.1 独立对拍（tools\test_protocol_v11.py）'
$python = Find-Python
if (-not $python) {
    Add-Failure '找不到 Python。请安装 Python 3.8+，或用 C:\Espressif\tools\python\python.exe'
} else {
    $env:PYTHONIOENCODING = 'utf-8'
    $pyArgs = @()
    if ([IO.Path]::GetFileNameWithoutExtension($python) -ieq 'py') {
        $pyArgs = @('-3')
    }
    $testScript = Join-Path $script:RepoRoot 'tools\test_protocol_v11.py'
    if (-not (Test-Path $testScript)) {
        Add-Failure ('找不到测试脚本: ' + $testScript)
    } else {
        Write-Host ('  使用 Python: ' + $python) -ForegroundColor DarkGray
        Push-Location $script:RepoRoot
        try {
            & $python @pyArgs 'tools\test_protocol_v11.py'
            $pyExit = $LASTEXITCODE
        } finally {
            Pop-Location
        }
        if ($pyExit -eq 0) {
            Write-Host '  Python 对拍全部通过 [OK]' -ForegroundColor Green
        } else {
            Add-Failure ('Python 对拍失败，退出码 ' + $pyExit)
        }
    }
}

# ---------------------------------------------------------------------------
# 汇总
# ---------------------------------------------------------------------------

Write-Step '测试结果汇总'
Write-Host ('  C 端 (test_protocol / test_ws_proto / test_linkcore / test_can_driver): ' + $(if ($SkipC) { '已跳过（-SkipC）' } elseif ($cExecuted) { '已执行并通过' } elseif ($cmake) { '未通过（见上方错误）' } else { '已跳过（缺少 cmake）' }))
Write-Host '  Python (test_protocol_v11.py):                          见上方输出'
Write-Host ''
if ($script:Failures.Count -eq 0) {
    Write-Host '  全部已执行的测试通过 [OK]' -ForegroundColor Green
    Write-Host '  提醒: 主机端测试通过 ≠ 上板验证通过；真实无线/UART/CAN/电机/供电必须按' -ForegroundColor Yellow
    Write-Host '        docs\通信可靠性与FreeRTOS移植说明.md 的分阶段步骤上板确认。' -ForegroundColor Yellow
    exit 0
}

Write-Host ('  有 ' + $script:Failures.Count + ' 项失败：') -ForegroundColor Red
foreach ($f in $script:Failures) { Write-Host ('    - ' + $f) -ForegroundColor Red }
exit 1
