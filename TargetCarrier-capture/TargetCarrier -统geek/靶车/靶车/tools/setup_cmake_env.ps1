# ============================================================================
#  setup_cmake_env.ps1 —— CMake 构建环境自检 / 安装 / 配置 (一键)
#
#  这个脚本只做四件事:
#      1. 找一个能用的 cmake (>= 3.25) / ninja / python
#      2. 找到 PlatformIO 的 packages 目录, 确认本工程需要的三个包都在
#      3. (-Install)   缺什么就装什么: 免安装 CMake zip + pip 装 ninja/pio
#      4. (-Configure) 体检通过后直接跑 cmake --preset esp32-master
#
#  为什么环境是这个样子, 见 docs\CMake构建说明.md 第一章:
#  一句话 —— CMake 负责编译, PlatformIO 只当 "SDK 仓库" 用,
#  因为 arduino-esp32 的框架 (几百 MB) 没有官方 CMake 包可装。
#
#  用法:
#      pwsh -File tools\setup_cmake_env.ps1                    # 只体检, 什么都不改
#      pwsh -File tools\setup_cmake_env.ps1 -Install           # 缺的自动装
#      pwsh -File tools\setup_cmake_env.ps1 -Configure         # 体检通过就配置
#      pwsh -File tools\setup_cmake_env.ps1 -Configure -Build  # 配置 + 编译
#      pwsh -File tools\setup_cmake_env.ps1 -PioPackagesDir D:\sdk\packages
#
#  注意: 用 pwsh (PowerShell 7) 运行最省事; Windows PowerShell 5.1 也能跑,
#        但它对无 BOM 的 UTF-8 中文会显示成乱码。
#
#  退出码: 0 = 环境就绪;  1 = 有缺项 (见输出);  2 = 用法/前置错误
# ============================================================================

[CmdletBinding()]
param(
    # 缺什么装什么 (CMake / Ninja / PlatformIO + espressif32 包)
    [switch]$Install,
    # 体检通过后执行 cmake --preset <Preset>
    [switch]$Configure,
    # 配置之后再 cmake --build --preset <Preset>
    [switch]$Build,
    # 预设名, 本工程是 esp32-master
    [string]$Preset = 'esp32-master',
    # 显式指定 PlatformIO packages 目录 (留空则自动探测)
    [string]$PioPackagesDir = ''
)

$ErrorActionPreference = 'Continue'
$script:missing = New-Object System.Collections.ArrayList

function Ok([string]$msg)   { Write-Host ('  [ OK ]  ' + $msg) -ForegroundColor Green }
function Bad([string]$msg)  { Write-Host ('  [MISS]  ' + $msg) -ForegroundColor Yellow; [void]$script:missing.Add($msg) }
function Info([string]$msg) { Write-Host ('          ' + $msg) -ForegroundColor DarkGray }
function Head([string]$msg) { Write-Host ("`n== " + $msg + ' ' + ('=' * [Math]::Max(0, 66 - $msg.Length))) -ForegroundColor Cyan }

# 在 PATH 里找, 找不到再看给定候选绝对路径
function Find-Exe([string]$name, [string[]]$candidates = @()) {
    $cmd = Get-Command $name -ErrorAction SilentlyContinue
    if ($cmd -and $cmd.Source) { return $cmd.Source }
    foreach ($c in $candidates) {
        if ($c -and (Test-Path -LiteralPath $c)) { return $c }
    }
    return $null
}

# pip --user 装出来的 exe 在 %APPDATA%\Python\PythonXY\Scripts, 扫描一遍
function Get-PyScriptsExes([string]$fileName) {
    $result = @()
    $pyRoot = Join-Path $env:APPDATA 'Python'
    if (Test-Path -LiteralPath $pyRoot) {
        foreach ($d in (Get-ChildItem -LiteralPath $pyRoot -Directory -ErrorAction SilentlyContinue)) {
            $result += (Join-Path $d.FullName ('Scripts\' + $fileName))
        }
    }
    $result += (Join-Path $env:APPDATA ('Python\Python312\Scripts\' + $fileName))
    return $result
}

# 把目录追加进 "用户级 PATH" (只在 -Install 时调用), 并让当前会话立即可用
function Add-UserPath([string]$dir) {
    if (-not $dir) { return }
    $dir = [System.IO.Path]::GetFullPath($dir).TrimEnd('\')
    if (-not (Test-Path -LiteralPath $dir)) { return }

    $userPath = [Environment]::GetEnvironmentVariable('Path', 'User')
    if (-not $userPath) { $userPath = '' }

    $already = $false
    foreach ($p in ($userPath -split ';')) {
        if ($p -and ($p.TrimEnd('\') -ieq $dir)) { $already = $true; break }
    }
    if (-not $already) {
        $joined = if ($userPath.Trim() -eq '') { $dir } else { $userPath.TrimEnd(';') + ';' + $dir }
        [Environment]::SetEnvironmentVariable('Path', $joined, 'User')
        Write-Host ('  已写入用户 PATH: ' + $dir + '   (新终端生效, 本窗口已临时生效)') -ForegroundColor DarkGray
    }
    if (($env:Path -split ';') -notcontains $dir) { $env:Path = $dir + ';' + $env:Path }
}

function Get-Version([string]$exe, [string[]]$exeArgs) {
    try {
        $text = & $exe @exeArgs 2>$null | Out-String
        if ($text -match '(\d+\.\d+(\.\d+)?)') { return [version]$Matches[1] }
    } catch { }
    return $null
}

$repoRoot   = Split-Path -Parent $PSScriptRoot
$masterDir  = Join-Path $repoRoot 'firmware\esp32_master'
$pkgNames   = @(
    'toolchain-xtensa-esp32s3',
    'framework-arduinoespressif32',
    'tool-esptoolpy'
)

# ============================================================================
#  1. 宿主机工具: cmake / ninja / python
# ============================================================================
Head '1/4  宿主机工具'

$cmakeCandidates = @(
    (Join-Path $env:LOCALAPPDATA 'Programs\CMake\bin\cmake.exe'),
    'C:\Program Files\CMake\bin\cmake.exe'
)
$ninjaCandidates = @(Get-PyScriptsExes 'ninja.exe')
$cmake  = Find-Exe 'cmake' $cmakeCandidates
$ninja  = Find-Exe 'ninja' $ninjaCandidates
$python = Find-Exe 'python' @()

# 找到但不在 PATH 里 (例如刚装完还没重开终端) -> 临时补进本会话 PATH
foreach ($exe in @($cmake, $ninja)) {
    if (-not $exe) { continue }
    $exeName = Split-Path -Leaf $exe
    if (-not (Get-Command $exeName -ErrorAction SilentlyContinue)) {
        $dir = Split-Path -Parent $exe
        if (($env:Path -split ';') -notcontains $dir) { $env:Path = $dir + ';' + $env:Path }
        Info ('临时加入本会话 PATH: ' + $dir)
    }
}

$cmakeVersion  = if ($cmake)  { Get-Version $cmake  @('--version') } else { $null }
$ninjaVersion  = if ($ninja)  { Get-Version $ninja  @('--version') } else { $null }

if ($cmake -and $cmakeVersion -and $cmakeVersion -ge [version]'3.25.0') {
    Ok ('cmake ' + $cmakeVersion + '   ' + $cmake)
} elseif ($cmake) {
    Bad ('cmake 版本太低 (' + $cmakeVersion + '), 需要 >= 3.25   ' + $cmake)
} else {
    Bad 'cmake 未安装'
}

if ($ninja) {
    Ok ('ninja ' + $ninjaVersion + '   ' + $ninja)
} else {
    Bad 'ninja 未安装 (CMake 预设用的是 Ninja 生成器)'
}

$pythonOk = $false
if ($python) {
    $pyVer = & $python -c "import sys; print('%d.%d.%d' % sys.version_info[:3])" 2>$null
    if ($LASTEXITCODE -eq 0 -and $pyVer) { $pythonOk = $true }
}
if ($pythonOk) {
    Ok ('python ' + $pyVer + '   ' + $python)
} else {
    Bad 'python 不可用 (esptool 生成 / 烧录镜像要用它)'
}

# ============================================================================
#  2. PlatformIO packages (只当 SDK 仓库用)
# ============================================================================
Head '2/4  PlatformIO packages (SDK 仓库)'

$pkgDir = ''
$pkgSource = ''
if ($PioPackagesDir) {
    $pkgDir = $PioPackagesDir
    $pkgSource = '-PioPackagesDir 参数'
} elseif ($env:PLATFORMIO_PACKAGES_DIR) {
    $pkgDir = $env:PLATFORMIO_PACKAGES_DIR
    $pkgSource = '环境变量 PLATFORMIO_PACKAGES_DIR'
} elseif ($env:PLATFORMIO_CORE_DIR) {
    $pkgDir = Join-Path $env:PLATFORMIO_CORE_DIR 'packages'
    $pkgSource = '环境变量 PLATFORMIO_CORE_DIR'
} else {
    $pkgDir = Join-Path $env:USERPROFILE '.platformio\packages'
    $pkgSource = '默认位置 %USERPROFILE%\.platformio\packages'
}

$pkgDir = [System.IO.Path]::GetFullPath($pkgDir).TrimEnd('\')
Info ('来源: ' + $pkgSource)

if (Test-Path -LiteralPath $pkgDir) {
    Ok ('packages 目录存在: ' + $pkgDir)
    foreach ($pkg in $pkgNames) {
        $p = Join-Path $pkgDir $pkg
        if (Test-Path -LiteralPath $p) { Ok ('  ' + $pkg) } else { Bad ('  缺少包 ' + $pkg) }
    }

    # 变体名要和 CMakePresets.json 里的 ESP32S3_VARIANT 对上
    $variant = Join-Path $pkgDir 'framework-arduinoespressif32\variants\m5stack_stamp_s3'
    if (Test-Path -LiteralPath $variant) {
        Ok '  变体 m5stack_stamp_s3'
    } elseif (Test-Path -LiteralPath (Join-Path $pkgDir 'framework-arduinoespressif32')) {
        Bad '  框架里没有 m5stack_stamp_s3 变体 (arduino-esp32 版本不对?)'
    }

    # esptool.py 是镜像生成/烧录的实际执行文件
    if (Test-Path -LiteralPath (Join-Path $pkgDir 'tool-esptoolpy\esptool.py')) {
        Ok '  tool-esptoolpy/esptool.py'
    } elseif (Test-Path -LiteralPath (Join-Path $pkgDir 'tool-esptoolpy')) {
        $alt = Get-ChildItem -LiteralPath (Join-Path $pkgDir 'tool-esptoolpy') -Filter 'esptool*.py' -ErrorAction SilentlyContinue
        if ($alt) { Ok ('  tool-esptoolpy/' + $alt[0].Name) } else { Bad '  tool-esptoolpy 里找不到 esptool.py' }
    }
} else {
    Bad ('packages 目录不存在: ' + $pkgDir)
    Info '装法: pip install --user platformio'
    Info '      python -m platformio pkg install --global --platform espressif32'
}

# ============================================================================
#  3. (-Install) 缺什么装什么
# ============================================================================
Head '3/4  安装 (-Install 时生效)'

if ($Install) {
    $needCmake = (-not $cmake) -or (-not $cmakeVersion) -or ($cmakeVersion -lt [version]'3.25.0')
    if ($needCmake) {
        # 和 docs\CMake构建说明.md 第二章一致的安装方式: 免安装 zip, 不需要管理员权限
        $cmakeVersionToInstall = '4.4.4'
        $zipUrl = 'https://cmake.org/files/v4.4/cmake-' + $cmakeVersionToInstall + '-windows-x86_64.zip'
        $zip = Join-Path $env:TEMP ('cmake-' + $cmakeVersionToInstall + '.zip')
        $dest = Join-Path $env:LOCALAPPDATA 'Programs'
        try {
            if (-not (Test-Path -LiteralPath $dest)) { New-Item -ItemType Directory -Path $dest -Force | Out-Null }
            Write-Host ('  下载 ' + $zipUrl) -ForegroundColor DarkGray
            Invoke-WebRequest -Uri $zipUrl -OutFile $zip -UseBasicParsing
            Write-Host '  解压 ...' -ForegroundColor DarkGray
            Expand-Archive -LiteralPath $zip -DestinationPath $dest -Force
            $extracted = Join-Path $dest ('cmake-' + $cmakeVersionToInstall + '-windows-x86_64')
            $target = Join-Path $dest 'CMake'
            if ((Test-Path -LiteralPath $extracted) -and -not (Test-Path -LiteralPath $target)) {
                Rename-Item -LiteralPath $extracted -NewName 'CMake'
            } elseif (Test-Path -LiteralPath $target) {
                Info ('已存在 ' + $target + ', 没有覆盖; 若要升级请先改名或删除它再重跑 -Install')
            }
            Add-UserPath (Join-Path $target 'bin')
            $cmake = Join-Path $target 'bin\cmake.exe'
            if (Test-Path -LiteralPath $cmake) {
                Ok ('cmake 已安装: ' + $cmake)
            } else {
                Write-Host '  cmake 解压后没找到 bin\cmake.exe, 请手动检查。' -ForegroundColor Red
            }
        } catch {
            Write-Host ('  CMake 自动安装失败: ' + $_.Exception.Message) -ForegroundColor Red
            Info '手动装: 从 https://cmake.org/download/ 下 zip, 解压到 %LOCALAPPDATA%\Programs\CMake'
        }
    } else {
        Ok 'cmake 已满足, 跳过'
    }

    if (-not $ninja -or -not $pythonOk) {
        if (-not $pythonOk) {
            Write-Host '  python 不可用, 无法用 pip 装 ninja/platformio。' -ForegroundColor Red
            Info '装 Python 3.12: winget install -e --id Python.Python.3.12'
        } else {
            Write-Host '  pip install --user ninja ...' -ForegroundColor DarkGray
            & $python -m pip install --user ninja
            if ($LASTEXITCODE -eq 0) {
                $ninja = Find-Exe 'ninja' (Get-PyScriptsExes 'ninja.exe')
                if ($ninja) { Add-UserPath (Split-Path -Parent $ninja) }
                if ($ninja) { Ok ('ninja 已安装: ' + $ninja) } else { Write-Host '  ninja 装完仍找不到, 可能只装了 wheel 没进 PATH。' -ForegroundColor Red }
            } else {
                Write-Host '  ninja 安装失败。' -ForegroundColor Red
            }
        }
    } else {
        Ok 'ninja 已满足, 跳过'
    }

    $frameworksOk = $true
    foreach ($pkg in $pkgNames) {
        if (-not (Test-Path -LiteralPath (Join-Path $pkgDir $pkg))) { $frameworksOk = $false }
    }
    if (-not $frameworksOk) {
        if (-not $pythonOk) {
            Write-Host '  python 不可用, 无法安装 PlatformIO 框架包。' -ForegroundColor Red
        } else {
            Write-Host '  pip install --user platformio ...' -ForegroundColor DarkGray
            & $python -m pip install --user platformio
            $pioExe = Find-Exe 'pio' (Get-PyScriptsExes 'pio.exe')
            if ($pioExe) { Add-UserPath (Split-Path -Parent $pioExe) }
            Write-Host '  platformio pkg install --global --platform espressif32 (几百 MB, 请耐心等) ...' -ForegroundColor DarkGray
            & $python -m platformio pkg install --global --platform espressif32
            if ($LASTEXITCODE -ne 0) {
                Write-Host '  框架包安装失败 (网络?)，可重试: python -m platformio pkg install --global --platform espressif32' -ForegroundColor Red
            }
        }
    } else {
        Ok 'PlatformIO 框架包已满足, 跳过'
    }

    # 装完重新判定一遍缺项, 汇总才准确
    $script:missing = New-Object System.Collections.ArrayList
    $cmakeVersion = if ($cmake -and (Test-Path -LiteralPath $cmake)) { Get-Version $cmake @('--version') } else { $null }
    if (-not $cmakeVersion) {
        Bad 'cmake 仍不可用'
    } elseif ($cmakeVersion -lt [version]'3.25.0') {
        Bad ('cmake 版本仍然太低 (' + $cmakeVersion + '), 需要 >= 3.25')
    }
    if (-not $ninja) { Bad 'ninja 仍不可用' }
    if (-not $pythonOk) { Bad 'python 仍不可用' }
    foreach ($pkg in $pkgNames) {
        if (-not (Test-Path -LiteralPath (Join-Path $pkgDir $pkg))) { Bad ('仍缺少 ' + $pkg) }
    }
} else {
    Info '未加 -Install, 不做任何安装。缺项可按上面提示手动装, 或加 -Install 自动装。'
}

# ============================================================================
#  4. (-Configure / -Build) 用预设真正配置 / 编译一次
# ============================================================================
if ($Build -and -not $Configure) {
    Write-Host "`n-Build 需要和 -Configure 一起用。" -ForegroundColor Red
    exit 2
}

if ($Configure) {
    Head ('4/4  cmake --preset ' + $Preset)

    if ($script:missing.Count -gt 0) {
        Write-Host '  环境还没齐, 先解决上面的 [MISS] 项再来配置。' -ForegroundColor Red
        exit 1
    }
    if (-not (Test-Path -LiteralPath (Join-Path $masterDir 'CMakePresets.json'))) {
        Write-Host ('  找不到 ' + (Join-Path $masterDir 'CMakePresets.json')) -ForegroundColor Red
        exit 2
    }

    $cmakeArgs = @('--preset', $Preset)
    # 只有用户显式指定 packages 目录时才写进缓存, 免得以后挪动 SDK 时缓存过期
    if ($PioPackagesDir) { $cmakeArgs += ('-DPIO_PACKAGES_DIR=' + $pkgDir) }

    $rc = 0
    Push-Location -LiteralPath $masterDir
    try {
        Write-Host ('  工作目录: ' + $masterDir) -ForegroundColor DarkGray
        Write-Host ('  ' + $cmake + ' ' + ($cmakeArgs -join ' ')) -ForegroundColor DarkGray
        & $cmake @cmakeArgs
        $rc = $LASTEXITCODE

        if ($rc -eq 0 -and $Build) {
            Write-Host ("`n  " + $cmake + ' --build --preset ' + $Preset) -ForegroundColor DarkGray
            & $cmake --build --preset $Preset
            $rc = $LASTEXITCODE
            if ($rc -eq 0) {
                $bin = Join-Path $masterDir ('build\firmware.bin')
                if (Test-Path -LiteralPath $bin) {
                    $size = (Get-Item -LiteralPath $bin).Length
                    Ok ('产物: ' + $bin + '  (' + ('{0:N0}' -f $size) + ' B)')
                }
            }
        }
    } finally {
        Pop-Location
    }

    if ($rc -ne 0) {
        Write-Host ("`n  失败 (exit " + $rc + ')。常见原因见 docs\CMake构建说明.md 第九、十节。') -ForegroundColor Red
        exit 1
    }
}

# ============================================================================
#  汇总
# ============================================================================
Head '结果'

if ($repoRoot -match '[^\x00-\x7F]') {
    Write-Host '  注意: 工程路径含非 ASCII 字符。' -ForegroundColor Yellow
    Info ('当前路径: ' + $repoRoot)
    Info '影响: Ninja 记录不了依赖, 每次构建都会全量重编 (只是慢, 结果正确)。'
    Info '解法: 整个工程拷到纯英文路径 (例如 D:\target-vehicle), 源码不用改。'
}

if ($script:missing.Count -eq 0) {
    Write-Host '  环境就绪。' -ForegroundColor Green
    Write-Host ''
    Write-Host '  接下来 (在本目录 firmware\esp32_master 下):'
    Write-Host '      cmake --preset esp32-master                 # 配置'
    Write-Host '      cmake --build --preset esp32-master         # 编译 -> build\firmware.bin'
    Write-Host '      cmake --build --preset esp32-master --target flash     # 烧录 (需接 USB)'
    Write-Host '      cmake --build --preset esp32-master --target monitor   # 看串口'
    Write-Host ''
    Write-Host '  或者: pwsh -File tools\setup_cmake_env.ps1 -Configure -Build'
    exit 0
} else {
    Write-Host ('  还缺 ' + $script:missing.Count + ' 项:') -ForegroundColor Yellow
    foreach ($m in $script:missing) { Write-Host ('    - ' + $m) -ForegroundColor Yellow }
    Write-Host ''
    Write-Host '  自动装: pwsh -File tools\setup_cmake_env.ps1 -Install'
    Write-Host '  手动装: docs\CMake构建说明.md 第二章'
    exit 1
}
