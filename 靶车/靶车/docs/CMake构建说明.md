# CMake 构建说明

本项目原来用 PlatformIO（`platformio.ini` + `pio run`），现已**全部改成 CMake**。
这份文档说明：装了什么、怎么用、以及为什么这么做。

> 老的 PlatformIO 配置没有丢，归档在 [`docs/legacy-platformio/`](legacy-platformio/) 里，
> 只是不再参与构建。

---

## 一、先搞清楚一件事：CMake 负责编译，PlatformIO 只当"SDK 仓库"

三块板都要 Arduino 核心库：

| 板子 | 需要的框架 | 大小 |
|---|---|---|
| ESP32-S3 | `framework-arduinoespressif32`（arduino-esp32 2.0.17 + 预编译的 ESP-IDF 4.4.7） | 数百 MB |
| STM32F103 | `framework-arduinoststm32`（STM32duino 2.x）+ `framework-cmsis` | 数百 MB |

这些框架官方**没有发布 CMake 包**（ESP-IDF 自己有，但 arduino-esp32 的
"Arduino as IDF component" 是另一套构建流程，和本项目原来的配法并不等价）。

所以这里的做法是：

```
        ┌─────────────────────────────────────────────┐
        │  CMake  ← 负责一切：编译、链接、生成镜像、烧录 │
        └──────────────────┬──────────────────────────┘
                           │ 只用它当 SDK 仓库
        ┌──────────────────▼──────────────────────────┐
        │  ~/.platformio/packages/                    │
        │     toolchain-xtensa-esp32s3/     ← 编译器   │
        │     toolchain-gccarmnoneeabi/     ← 编译器   │
        │     framework-arduinoespressif32/ ← 源码+库  │
        │     framework-arduinoststm32/     ← 源码+库  │
        │     tool-esptoolpy/  tool-openocd/           │
        └─────────────────────────────────────────────┘
```

好处：

1. 不用重新下载几百 MB 的框架，机器上已有的直接用；
2. 编译参数、宏、头文件搜索路径都和原来的 PlatformIO 构建**逐项对齐**
   （做法见第八节），产物等价；
3. 想彻底甩掉 PlatformIO 也很简单：把这几个目录拷到任意位置，然后
   `cmake -DPIO_PACKAGES_DIR=<新位置>/packages ...` 指过去就行。

---

## 二、环境安装（已经装好了，这里是记录）

| 组件 | 版本 | 位置 |
|---|---|---|
| CMake | 4.4.4 | `%LOCALAPPDATA%\Programs\CMake\bin\cmake.exe` |
| Ninja | 1.13.2 | `%APPDATA%\Python\Python312\Scripts\ninja.exe` |

两者都已加入**用户级 PATH**，新开的终端里可以直接用。

> ⚠ **已经在运行的 VS Code 需要重启一次**，否则它还是用旧的环境变量，
> 会报 `'cmake' 不是内部或外部命令`。重开窗口（关闭整个 VS Code 再打开）即可，
> 不用重启电脑。

验证：

```powershell
cmake --version      # 应输出 cmake version 4.4.4
ninja --version      # 应输出 1.13.2
```

### 换一台机器怎么重装

```powershell
# 1. CMake：从 cmake.org 下 zip（免安装，不需要管理员权限）
Invoke-WebRequest https://cmake.org/files/v4.4/cmake-4.4.4-windows-x86_64.zip -OutFile $env:TEMP\cmake.zip
Expand-Archive $env:TEMP\cmake.zip "$env:LOCALAPPDATA\Programs" -Force
Rename-Item "$env:LOCALAPPDATA\Programs\cmake-4.4.4-windows-x86_64" "$env:LOCALAPPDATA\Programs\CMake"
[Environment]::SetEnvironmentVariable('Path', "$env:LOCALAPPDATA\Programs\CMake\bin;" + [Environment]::GetEnvironmentVariable('Path','User'), 'User')

# 2. Ninja：一条 pip 搞定
python -m pip install --user ninja

# 3. 框架与工具链：PlatformIO 已经装过就不用管，没装过就先装
pip install platformio
pio pkg install --global --platform espressif32
pio pkg install --global --platform ststm32
```

> `PIO_PACKAGES_DIR` 也可以用环境变量 `PLATFORMIO_PACKAGES_DIR` 覆盖，
> 或者 `-DPIO_PACKAGES_DIR=...` 传给 cmake。

---

## 三、目录结构

```
靶车/
├── CMakeLists.txt              ← 顶层工程：协议自测 + 串起三个固件（ExternalProject）
├── CMakePresets.json           ← 顶层预设 "all"
├── cmake/
│   ├── pio_sdk.cmake           ← 在哪儿找 PlatformIO 的 packages
│   ├── toolchain-esp32s3.cmake ← Xtensa 交叉工具链
│   ├── toolchain-stm32f103.cmake ← ARM 交叉工具链
│   ├── esp32_arduino.cmake     ← arduino-esp32 的 CMake 支持（core / WiFi / 镜像 / 烧录）
│   ├── esp32s3_arduino_sdk.cmake ← 自动派生：199 条 IDF 头文件路径 + 21 个宏
│   ├── stm32_arduino.cmake     ← STM32duino 的 CMake 支持
│   └── (串口监视器不是 cmake 文件，用的是 ../tools/serial_monitor.py)
├── firmware/
│   ├── lib/
│   │   ├── CMakeLists.txt      ← protocol + oled 两个共享静态库
│   │   ├── protocol/           ← 三端共用协议（唯一真源）
│   │   └── oled/
│   ├── esp32_master/
│   │   ├── CMakeLists.txt
│   │   └── CMakePresets.json   ← 预设 "esp32-master"
│   ├── esp32_bridge/
│   │   ├── CMakeLists.txt
│   │   └── CMakePresets.json   ← 预设 "esp32-bridge"
│   └── stm32_f103/
│       ├── CMakeLists.txt
│       └── CMakePresets.json   ← 预设 "stm32-f103"
└── tools/
    ├── serial_monitor.py       ← monitor 目标用的极简串口监视器
    ├── pc_monitor.py           ← PC 端串口监视 + 协议对拍
    ├── sim_tuning.py
    └── verify_all.ps1          ← 一键验证（调 ctest + python + cmake）
```

---

## 四、VS Code 用法（推荐）

### 需要的扩展

```json
"ms-vscode.cmake-tools",      // 必须
"ms-vscode.cpptools",         // 必须（IntelliSense）
"marus25.cortex-debug"        // 可选，STM32 在线调试
```

打开某个固件文件夹时，`.vscode/extensions.json` 会提示你装。

### 三步走

1. **用 VS Code 打开对应固件文件夹**，例如 `firmware/esp32_master`
   （★ 不要用仓库根目录去烧板子 —— 根目录只能"编全部"，没有单板的 flash 任务）
2. 底部状态栏点 **"未选择工具包 / No Kit Selected"** → 选预设
   `esp32-master` / `esp32-bridge` / `stm32-f103`
   （如果状态栏显示的是 "CMake: [Unconfigured]"，点一下选 "Use CMakePresets"）
3. **Build**（`F7`），需要烧录时用 **Terminal → Run Task → cmake: flash**

`Ctrl+Shift+B` 直接触发默认的 `cmake: build` 任务。

### 预设从哪来

每个固件目录下都有 `CMakePresets.json`，里面写死了：

- `generator`：Ninja
- `binaryDir`：构建目录
- `toolchainFile`：用哪套交叉工具链

所以**选对预设 = 选对工具链**，不会有"用宿主 gcc 编 ESP32"这种事。
如果你手敲 `cmake -S . -B build` 而没走预设，CMakeLists 会直接报错并提示你该怎么做。

---

## 五、命令行用法

### 单块板（最常用）

| 工程 | 预设 | 构建目录 |
|---|---|---|
| `firmware/esp32_master` | `esp32-master` | `firmware/esp32_master/build` |
| `firmware/esp32_bridge` | `esp32-bridge` | `firmware/esp32_bridge/build` |
| `firmware/stm32_f103` | `stm32-f103` | `C:/cmake_build/target-vehicle/stm32` |

```bash
cd firmware/esp32_master
cmake --preset esp32-master                 # 配置
cmake --build --preset esp32-master         # 编译
cmake --build --preset esp32-master --target flash     # 烧录
cmake --build --preset esp32-master --target monitor   # 看串口
```

> ★ 关于路径的两条硬性约束（都验证过）：
> 1. **STM32 的构建目录必须是纯 ASCII** —— Windows 的 GNU binutils（`arm-none-eabi-objcopy`）
>    不认 UTF-8 路径，路径含中文时链接能过、但转 `.bin/.hex` 会失败。预设已指到
>    `C:/cmake_build/target-vehicle/stm32`。
> 2. **建议整个工程目录也用纯英文名** —— 否则 Ninja 记录不了依赖，每次构建都全量重编。
>    这一条只影响速度，不影响正确性，配置时会给警告。详见第十节。

### 全部（顶层）

```bash
cmake --preset all
cmake --build --preset all      # 三个固件 + 协议自测程序
ctest --preset all              # 跑协议库自测
cmake --build --preset all --target firmware-info   # 打印三个 .bin 的位置
```

顶层用 `ExternalProject` 依次调用三个子工程，所以本质上还是上面那三步，
只是帮你一次跑完。**烧录请回到各自的固件目录**（顶层没有 flash 目标）。

---

## 六、自定义目标一览

| 目标 | ESP32（master / bridge） | STM32 |
|---|---|---|
| `flash` | ✅ esptool 写 4 个区（bootloader/分区/boot_app0/应用） | ✅ OpenOCD + ST-Link |
| `erase` | ✅ 整片擦除 | — |
| `monitor` | ✅ `tools/serial_monitor.py`（主机 460800 / 桥接 115200） | — （串口被链路占用） |
| `size` | ✅ `xtensa-esp32s3-elf-size` | ✅ `arm-none-eabi-size` |
| `reset` | — | ✅ 只复位不重烧 |

### 烧录串口怎么指定

esptool 会自己扫描串口；想指定就把环境变量设上再跑：

```powershell
$env:ESP32_PORT = "COM5"
cmake --build --preset esp32-master --target flash
```

或者配置时就写进缓存：

```powershell
cmake --preset esp32-master -DESP32_PORT=COM5
```

STM32 用 OpenOCD + ST-Link，不需要串口；换下载器就改
`cmake/stm32_arduino.cmake` 里 `flash` 目标的 `-f interface/xxx.cfg`：

| 下载器 | interface 配置 |
|---|---|
| ST-Link | `interface/stlink.cfg`（当前默认） |
| DAPLink | `interface/cmsis-dap.cfg` |
| J-Link | `interface/jlink.cfg`（需另装 J-Link 驱动） |

---

## 七、产物对照

| 产物 | 位置 | 说明 |
|---|---|---|
| `firmware.elf` | 构建目录 | 带调试信息，给调试器 / `size` 用 |
| `firmware.bin` | 构建目录 | ★ 就是要烧进去的那个 |
| `firmware.hex` | 构建目录（仅 STM32） | 串口 ISP 烧录用 |
| `firmware.map` | 构建目录 | 卡住时看符号都占了哪块 Flash |
| `bootloader.bin` / `partitions.bin` / `boot_app0.bin` | ESP32 构建目录 | 和 `firmware.bin` 一起按偏移烧 |

---

## 八、等价性：怎么确认"换成 CMake 没改变行为"

这不是"看起来能编过"，而是逐项对齐后验证的：

### 1. 编译参数是抓出来的，不是猜出来的

用 `pio run -v` 把 PlatformIO 的真实编译命令行全部抓下来，再从里面导出：

- 199 条 `-I`（IDF 组件头文件路径）→ `cmake/esp32s3_arduino_sdk.cmake`
- 21 个 `-D`（宏）
- C / C++ 各自的 `-std`、`-Os`、`-mlongcalls`、`-ffunction-sections` …

STM32 那边同理：27 条 `-I`、15 个 `-D`、28 个 core 源文件。

### 2. 关掉了 CMake 自己会加的东西

`CMAKE_BUILD_TYPE=Release` 会追加 `-O3 -DNDEBUG`，而 PlatformIO 的 ESP32 构建
**只有 `-Os`、没有 `NDEBUG`**。差别不在性能而在 `assert()` 行为，所以
`esp32_arduino.cmake` / `stm32_arduino.cmake` 里把内置的
`CMAKE_C_FLAGS_<CONFIG>` 全部清空了，优化级别只由显式的 `-Os` 决定。

### 3. 结果对照

| 项目 | PlatformIO | CMake | 结论 |
|---|---|---|---|
| ESP32 主机 `bootloader.bin` | — | — | **SHA256 完全一致** |
| ESP32 主机 `partitions.bin` | — | — | **SHA256 完全一致** |
| ESP32 主机 `firmware.bin` | 676 000 B | 676 608 B | 差 0.09% |
| ESP32 主机外部符号表 | 5 736 个 | 5 736 个 | **差集为 0** |
| ESP32 桥接 `firmware.bin` | 679 408 B | 679 824 B | 差 0.06% |
| STM32 `firmware.bin` | 22 292 B | 22 292 B | **字节数完全相同** |
| STM32 外部符号表 | 341 个 | 341 个 | **差集为 0** |

**怎么解释那几十～几百字节的差？**
两边的源文件、宏、优化选项完全一致，编译出的 `.o` 是一样的；差别来自
**链接顺序**。`--gc-sections` 之后各段的排布、对齐填充会随顺序变，
所以 `.bin` 的字节内容不同、但**符号集合完全相同**（已用
`nm --defined-only --extern-only` 逐符号比对，差集为 0）。
换句话说：同一份程序，换了个排布。

要自己复核：

```bash
# 符号表对比
arm-none-eabi-nm --defined-only --extern-only <elf> | awk '{print $3}' | sort -u > a.txt
diff a.txt b.txt
```

---

## 九、踩过的坑（都写在对应文件里了）

| 现象 | 原因 | 处理 |
|---|---|---|
| **每次构建都全量重编**，第二次构建也不输出 `ninja: no work to do` | **源码目录含中文** —— Ninja 在 Windows 上无法把 `.d` 依赖文件里带中文的路径对应回文件，每次判定 `deps are missing` | 把工程目录改成纯英文名（如 `D:\target-vehicle`）。配置时会给出警告；实测对照：ASCII 路径下第二次构建是 `no work to do`，中文路径下是 `[13/13]` 全编 |
| `xtensa-...-gcc.elf is not a full path` | 工具链文件里用 `CMAKE_EXECUTABLE_SUFFIX` 拼宿主机 exe 后缀，而它后面被改成了 `.elf` | 用单独的 `_host_exe` 变量 |
| 199 条 `-I` 变成 `/tools/sdk/...` | 数据表在 `ESP32S3_ARDUINO_ROOT` 赋值**之前** include，`${}` 提前展开成空 | 先赋值再 include（`esp32_arduino.cmake` 里有注释） |
| `Feature 'RESCAN' ... not defined for the CXX link language` | `CMAKE_SYSTEM_NAME=Generic` 时 CMake 不认 `$<LINK_GROUP:RESCAN>` | 按语言显式定义 `CMAKE_<LANG>_LINK_GROUP_USING_RESCAN`，值要带 `-Wl,` 前缀 |
| `unrecognized command line option '--start-group'` | 上面那个值写成了裸 `--start-group`，被直接喂给了编译器而不是链接器 | 改成 `"-Wl,--start-group"` |
| `arm-none-eabi-objcopy.exe: .../firmware.elf: No such file or directory`（路径里是乱码） | 构建目录含中文。Windows 的 GNU binutils 不认 UTF-8 路径 —— 链接其实成功了，卡在 ELF→`.bin/.hex` 的 objcopy。ESP32 没这问题，因为镜像由 `esptool.py`（Python）生成 | STM32 预设把 `binaryDir` 指到 `C:/cmake_build/target-vehicle/stm32` |
| `cannot open linker script file "C:/...` | 链接选项写成 `-T"<路径>"`，CMake 生成 `\"`，而 cmd.exe 不认 `\"` | 改成 `-Wl,-T,<路径>` |
| `warning: cannot find entry symbol Reset_Handler`，镜像只有 7 KB | core 的 glob 漏了 `*.S` —— 中断向量表在 `startup_stm32yyxx.S` 里（同目录的 `.c` 在 F1 下编译出来是空的） | glob 加上 `*.S` |
| `monitor` 报找不到 `tools/serial_monitor.py` | 在 CMake 函数体里 `CMAKE_CURRENT_LIST_DIR` 指的是**调用者**的目录 | 改用 `CMAKE_CURRENT_FUNCTION_LIST_DIR` |
| 每次构建都提示 `Re-running CMake` | `file(GLOB ... CONFIGURE_DEPENDS)` 会插入一个每次构建都跑的 glob 校验 | 去掉 `CONFIGURE_DEPENDS`（本项目自己的源文件都是显式列出的，不受影响） |
| `ctest` 报 `Test not available without configuration` | 顶层用的是多配置生成器（Visual Studio） | `CMakePresets.json` 的 testPresets 里加 `"configuration"` |

---

## 十、常见问题

**Q: 为什么每次构建都全量重编？第二次构建也不打印 `ninja: no work to do`？**
因为**工程目录名是中文**（`C:\Users\zzq\Desktop\靶车`）。Ninja 在 Windows 上读不了
`.d` 依赖文件里带中文的路径，每次构建都判定 `deps are missing`，于是把所有源文件
重编一遍。编译结果是正确的，纯粹是慢（STM32 约 40 秒，ESP32 几分钟）。

我做过对照实验确认：

| 工程位置 | 第二次构建的输出 |
|---|---|
| `C:\target-vehicle`（纯 ASCII） | `ninja: no work to do.` ✅ |
| `C:\Users\zzq\Desktop\靶车`（中文） | `[13/13]` 全部重编 ❌ |

解决办法只有一个：**把工程目录改成纯英文名**，例如 `D:\target-vehicle`。
源码内容、CMakeLists、预设都不用改（内部路径全是相对的）。
配置时 CMake 会专门给一条警告提醒这件事。

**Q: 我只想编译，不想装任何东西。**
框架和工具链已经在 `~/.platformio/packages` 了，只要装上 CMake + Ninja 就能编，
不需要装 PlatformIO 再来一遍。

**Q: 能不能不依赖 PlatformIO 的目录？**
可以，把 `framework-arduinoespressif32`、`framework-arduinoststm32`、
`framework-cmsis`、`toolchain-xtensa-esp32s3`、`toolchain-gccarmnoneeabi`、
`tool-esptoolpy`、`tool-openocd` 这七个目录整体拷走，然后
`-DPIO_PACKAGES_DIR=<新位置>/packages`。

**Q: 升级了 PlatformIO 的 ESP32 框架怎么办？**
`cmake/esp32s3_arduino_sdk.cmake` 里的 199 条路径是按 arduino-esp32 2.0.17 导出的。
换大版本后需要重新导出（步骤见文件头注释），否则可能缺头文件。

**Q: CMake 版本低于 3.25 行不行？**
不行。用到两个较新的特性：`$<LINK_GROUP:RESCAN>`（3.24 引入）和
`CMakePresets.json` 的 schema version 6（3.25 引入，顶层用到了 workflowPresets）。
所有 `CMakeLists.txt` 与 `CMakePresets.json` 里都写了 `3.25` 作为下限。
