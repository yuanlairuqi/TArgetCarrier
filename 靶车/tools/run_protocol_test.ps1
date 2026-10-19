# ============================================================================
#  靶车 —— 协议库主机端自测
#
#  在电脑上直接编译并运行 firmware/lib/protocol/test/test_protocol.c,
#  不需要插任何硬件。改了 protocol.h 之后务必先跑这个, 再烧板子。
#
#  用法:
#     pwsh -File tools\run_protocol_test.ps1
#
#  依赖: 本机已安装 Visual Studio (含 C++ 工具集) + Windows SDK。
#        找不到编译器时脚本会给出提示, 不会静默失败。
# ============================================================================

$ErrorActionPreference = 'Stop'

$repoRoot = Split-Path -Parent $PSScriptRoot
$protoDir = Join-Path $repoRoot 'firmware\lib\protocol'
$buildDir = Join-Path $protoDir 'test\build'

# ---- 1. 定位编译器 (不依赖 vcvarsall, 直接把路径拼出来) ----
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$clPath  = $null

if (Test-Path $vswhere) {
    $vsRoot = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    if ($vsRoot) {
        $clPath = Get-ChildItem (Join-Path $vsRoot 'VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe') -ErrorAction SilentlyContinue |
                  Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $clPath) {
    $clPath = Get-ChildItem 'C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\cl.exe' -ErrorAction SilentlyContinue |
              Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
}
if (-not $clPath) {
    Write-Host '找不到 MSVC 编译器 (cl.exe)。' -ForegroundColor Red
    Write-Host '请安装 Visual Studio 的「使用 C++ 的桌面开发」工作负载, 或改用 ARM/ESP 工具链交叉编译本测试。'
    exit 2
}
$vcBin = Split-Path -Parent $clPath
$msvcRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $vcBin))   # ...\MSVC\<ver>

# ---- 2. Windows SDK ----
$sdkInc = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Include\*' -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
$sdkLib = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\Lib\*' -Directory -ErrorAction SilentlyContinue |
          Sort-Object Name -Descending | Select-Object -First 1
if (-not $sdkInc -or -not $sdkLib) {
    Write-Host '找不到 Windows SDK。' -ForegroundColor Red
    exit 2
}

$env:INCLUDE = @(
    "$($sdkInc.FullName)\ucrt"
    "$($sdkInc.FullName)\um"
    "$($sdkInc.FullName)\shared"
    "$msvcRoot\include"
) -join ';'

$env:LIB = @(
    "$($sdkLib.FullName)\ucrt\x64"
    "$($sdkLib.FullName)\um\x64"
    "$msvcRoot\lib\x64"
) -join ';'

$env:PATH = "$vcBin;$env:PATH"

New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

# ---- 3. 编译 ----
Write-Host "编译器: $clPath" -ForegroundColor DarkGray
Write-Host "工程:   $protoDir" -ForegroundColor DarkGray

Push-Location $protoDir
try {
    # /W4 视为硬要求: 协议头要能过最高告警等级
    & $clPath /nologo /utf-8 /W4 /I. `
        /Fe:"$buildDir\test_protocol.exe" `
        /Fo:"$buildDir\" `
        'test\test_protocol.c'

    if ($LASTEXITCODE -ne 0) {
        Write-Host "编译失败 (exit $LASTEXITCODE)" -ForegroundColor Red
        exit 1
    }

    # ---- 4. 运行 ----
    & "$buildDir\test_protocol.exe"
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
