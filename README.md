# 靶车 · ESP-NOW → CAN 云台控制系统

用一颗 ESP32 通过 ESP-NOW 控制达妙 DM-G6220 中空云台电机的完整实现。
链路走的是 **ESP-NOW → UART → CAN** 三级转发，不是透传，每一级都有协议校验和失联保护。

```
 ┌──────────────┐   ESP-NOW     ┌────────────────┐   UART      ┌──────────────┐   CAN      ┌───────────┐
 │  ESP32 主机   │  2.4G 广播     │  M5Stamp S3    │  921600     │ STM32F103C8T6│  1 Mbps    │ DM-G6220  │
 │ 自主轨迹发生器 │ ────────────► │  桥接/看门狗    │ ──────────► │  MIT 闭环    │ ─────────► │ 云台电机   │
 │  (USB 调参)   │   50 Hz       │  (USB 调试口)   │   50 Hz     │   1 kHz     │            │  ID = 1   │
 └──────────────┘               └────────────────┘             └──────────────┘            └───────────┘
```

---

## 目录结构

构建体系已从 PlatformIO 全部迁到 **CMake**（`cmake --preset xxx` 即可编译烧录），
细节见 [CMake 构建说明](docs/CMake构建说明.md)。

```
靶车/
├── README.md                        ← 你正在看的这份
├── CMakeLists.txt                   ← 顶层工程：协议自测 + 串起三个固件
├── CMakePresets.json                ← 顶层预设 "all"
├── cmake/                           ← 构建体系的公共部分
│   ├── pio_sdk.cmake                定位 PlatformIO 的 packages（当 SDK 仓库用）
│   ├── toolchain-esp32s3.cmake      Xtensa 交叉工具链
│   ├── toolchain-stm32f103.cmake    ARM 交叉工具链
│   ├── esp32_arduino.cmake          arduino-esp32 的 CMake 支持
│   ├── esp32s3_arduino_sdk.cmake    199 条 IDF 头文件路径 + 21 个宏（自动派生）
│   └── stm32_arduino.cmake          STM32duino 的 CMake 支持
├── docs/
│   ├── 接线与调试手册.md              ← ★ 接线表、烧录、分步验证、排错、协议速查
│   ├── CMake构建说明.md              ← ★ 环境安装、预设、自定义目标、等价性验证
│   └── legacy-platformio/           迁移前的 platformio.ini（仅存档，不参与构建）
├── firmware/
│   ├── lib/                         ← 三端共用库
│   │   ├── CMakeLists.txt
│   │   ├── protocol/                ← ★ 协议库（唯一真源）
│   │   │   ├── protocol.h           帧格式 / CRC16 / 编解码 / 编译期断言
│   │   │   ├── protocol.c           锚点 + _Static_assert 长度自检
│   │   │   └── test/test_protocol.c 主机端自测（不接硬件）
│   │   └── oled/                    128x64 OLED 驱动（硬件无关）
│   ├── esp32_master/                ← ESP32 主机（发射端，自主轨迹）
│   │   ├── CMakeLists.txt
│   │   ├── CMakePresets.json        预设 "esp32-master"
│   │   ├── include/app_config.h     链路/轨迹/增益参数
│   │   ├── include/motion_profile.h
│   │   └── src/{main.cpp, motion_profile.c}
│   ├── esp32_bridge/                ← M5Stamp S3 桥接端
│   │   ├── CMakeLists.txt
│   │   ├── CMakePresets.json        预设 "esp32-bridge"
│   │   ├── include/app_config.h     UART 引脚/波特率/看门狗
│   │   └── src/main.cpp             ESP-NOW 回调 → 队列 → 转发 + 看门狗
│   └── stm32_f103/                  ← STM32F103C8T6 控制器
│       ├── CMakeLists.txt
│       ├── CMakePresets.json        预设 "stm32-f103"（构建目录在纯 ASCII 路径）
│       ├── include/{config.h, app_main.h, can_drv.h, dm_motor.h,
│       │           link_uart.h, timer_drv.h}
│       └── src/{main.cpp, app_main.c, can_drv.c, dm_motor.c,
│                link_uart.cpp, timer_drv.cpp}
├── tools/
│   ├── run_protocol_test.ps1        协议自测（CMake 薄封装，一键跑）
│   ├── verify_all.ps1               一键验证：编译三块板 + 协议自测 + Python 对拍
│   ├── serial_monitor.py            monitor 目标用的串口监视器
│   ├── pc_monitor.py                PC 端串口监视 + 独立协议对拍
│   └── sim_tuning.py                增益/轨迹离线仿真
└── 资料/                            你提供的四份原始资料 + 解压内容
```

---

## 快速开始

### 0. 先看不接硬件的自测（强烈建议）

```powershell
# 一键验证：编译三块板 + C 端协议自测 + Python 端协议对拍
pwsh -File tools\verify_all.ps1

# 或者只跑协议自测（等价的纯 CMake 写法见 docs/CMake构建说明.md）
pwsh -File tools\run_protocol_test.ps1
```

两边的帧格式与 CRC 全绿，说明三端一致，可以放心烧板子。

### 1. 编译烧录（VS Code，推荐）

**环境**：装好 CMake + Ninja 即可（本机已装，见
[CMake 构建说明](docs/CMake构建说明.md) 第二章）。框架和交叉工具链复用
PlatformIO 已下载的那份，不需要额外装。

每个固件都是独立工程，用 VS Code 打开对应文件夹，状态栏选预设，然后：

| 工程 | 预设名 | 构建目录 | Build (F7) | 烧录 / 看串口 |
|---|---|---|---|---|
| `firmware/esp32_master` | `esp32-master` | `<工程>/build` | ✅ | 任务 `cmake: flash` / `cmake: monitor`（460800）|
| `firmware/esp32_bridge` | `esp32-bridge` | `<工程>/build` | ✅ | 任务 `cmake: flash` / `cmake: monitor`（115200）|
| `firmware/stm32_f103` | `stm32-f103` | `C:/cmake_build/target-vehicle/stm32` | ✅ | 任务 `cmake: flash`（ST-Link）|

命令行等价：

```bash
cd firmware/esp32_master
cmake --preset esp32-master                    # 配置
cmake --build --preset esp32-master            # 编译
cmake --build --preset esp32-master --target flash     # 烧录
```

三块板一起编译（不烧录）：

```bash
cmake --preset all
cmake --build --preset all
ctest --preset all
```

### 2. 接线

**只做三件事，细节看 [接线与调试手册](docs/接线与调试手册.md)：**

- 桥接 `G1 → STM32 PA10`，桥接 `G2 ← STM32 PA9`，**并且共地**
- STM32 **`PB9` → SN65HVD230 CTX**，**`PB8` ← CRX**，模块用 **3V3** 供电
  （★ **不是 PA11/PA12**：野火双 USB 板把 PA11/PA12 焊到 USB 插座上了，必须用重映射的 PB8/PB9）
- CANH/CANL 接电机，**总线两端各一个 120Ω 终端电阻**

**可选：两块 OLED 状态屏（各挂各的 I2C，互不影响）**

| 屏 | SCL | SDA | VCC |
|---|---|---|---|
| → STM32 | **PB6** | **PB7** | 3V3 |
| → M5Stamp S3 | **G9** | **G8** | 3V3 |

没接屏幕不会影响控制 —— `display_init()` 返回 false，刷新自动跳过。

### 3. 上电验证

看**桥接端**（M5Stamp）的串口，正常应该是这样一行：

```
[桥接] 在线 rssi=-45 | 主机 ok=452 bad=0 dup=0 | STM32 yaw=0.00 pitch=0.00 T=28C err=0 | 转发=452
```

- `在线` → ESP-NOW 通了
- `STM32 yaw=...` → 串口链路双向通了
- `yaw` 跟着目标变 → CAN 和电机都通了

然后打开**主机端**串口，敲 `1` 让它开始正弦摆动。

---

## 设计要点

### 三层失联保护（任何一层单独失效都不会让云台失控）

| 层 | 位置 | 行为 |
|---|---|---|
| 1 | 桥接端 | 主机 300 ms 无有效帧 → 每 50 ms 注入失能帧（无 ENABLE 位） |
| 2 | STM32 | 自己也有独立 300 ms 看门狗，超时立即 kp/kd 归零 + 连发失能命令 |
| 3 | 增益斜坡 | 使能 500 ms 斜坡，失能 200 ms 收敛，避免阶跃把电机拽响 |

第 2 层是关键：它**不依赖**桥接端，即使桥接死机或串口断线，STM32 也会自己松掉电机。

### 协议帧格式

**控制帧（17 字节，主机 → 桥接 → STM32）**

```
0    1    header 0xAA
1    1    len = 13
2    1    type = 0x01
3    1    seq（序号，用于丢弃重复/乱序帧）
4..5 2    flags（ENABLE / CLEAR_ERR / SET_ZERO / ESTOP）
6    1    mode（MIT / POS / SPD / PSI）
7..8 2    yaw  量化值 = 度 / 0.01
9..10 2   pitch 量化值
11..12 2  yaw_rate 量化值 = dps / 0.1
13..14 2  pitch_rate 量化值
15   1    CRC16-Modbus(bytes[1..14]) 低 8 位
16   1    0x55 帧尾
```

**反馈帧（17 字节，STM32 → 桥接）**

```
0    1    0x5A
1    1    len = 15
2    1    type = 0x02
3    1    seq
4    1    rssi
5    1    state（INIT/OK/TIMEOUT/ESTOP/FAULT）
6..9  4    float yaw_deg（小端 IEEE754）
10..13 4   float pitch_deg
14   1    temp_mos
15   1    err_code
16   1    CRC16-Modbus(bytes[1..15])
```

> 反馈帧是 **17** 字节而不是 16。曾经按 16 写会把 CRC 写到 `temp_mos` 上（静默丢数据），
> 已被 `test_protocol.c` 的字节级断言锁死，不会再回归。

### 为什么三端共用一个 `protocol.h`

帧格式如果每端各写一份，改一处忘一处就是"主机说通了、板子上不通"的经典问题。
所以三端都通过 `firmware/lib/CMakeLists.txt` 里的同一个 `protocol` 静态库引用**同一份文件**，
并且 `protocol.c` 里有 `_Static_assert` 长度自检 —— 谁改了帧长，**编译期就报错**，不会拖到现场。

---

## 已验证的内容

| 项目 | 状态 |
|---|---|
| 协议库 C 端自测（长度/往返/限幅/坏帧/CRC 标准向量/四种模式） | ✅ 全绿（`ctest --preset all`）|
| 协议库 Python 端独立对拍 | ✅ 全绿 |
| **构建体系迁移：PlatformIO → CMake** | ✅ 三块板全部可编、可烧 |
| ESP32 主机固件编译（CMake，ESP32-S3） | ✅ 通过 |
| 桥接固件编译（CMake，ESP32-S3） | ✅ 通过 |
| STM32F103 固件编译（CMake，零警告） | ✅ 通过，Flash 22 292 B |
| **与旧 PlatformIO 产物逐项对照** | ✅ 符号表差集为 0（详见下）|

### CMake 与 PlatformIO 产物对照

| 项目 | PlatformIO | CMake | 结论 |
|---|---|---|---|
| ESP32 主机 `bootloader.bin` / `partitions.bin` | — | — | **SHA256 完全一致** |
| ESP32 主机 `firmware.bin` | 676 000 B | 676 608 B | 差 0.09% |
| ESP32 主机外部符号表 | 5 736 个 | 5 736 个 | **差集为 0** |
| ESP32 桥接 `firmware.bin` | 679 408 B | 679 824 B | 差 0.06% |
| STM32 `firmware.bin` | 22 292 B | 22 292 B | **字节数完全相同** |
| STM32 外部符号表 | 341 个 | 341 个 | **差集为 0** |

差的那几十～几百字节是**链接顺序**造成的段排布差异（同一份程序换了个摆法），
符号集合逐项比对差集为 0。复核方法与原理见
[CMake 构建说明](docs/CMake构建说明.md) 第八节。

**尚未在真实硬件上验证的部分**（拿到板子后按手册第四章分步验证）：

- ESP-NOW 实链路质量与距离
- STM32 串口实际收发时序
- CAN 与 DM-G6220 的实际闭环表现、kp/kd 手感
- 电机实际量程（PMAX/VMAX/TMAX）是否与 `config.h` 一致 —— **这一项务必用达妙 DMTool 核对**

---

## 开发过程中踩到并已修掉的坑

留在这里，避免重复踩：

1. **反馈帧长度算错**（16 → 17）。字段加起来放不下，CRC 覆盖了 `temp_mos`。
   现在有字节级断言 + Python 对拍双重保护。

2. **`#pragma pack(1)` 在不同编译器下不一致**。MSVC 在 C 模式下会忽略它
   （uint16+uint8+int16 变成 11 字节），GCC 是 9 字节。已把该结构体与线格式解耦，
   改用显式字节偏移编解码。

3. **STM32duino 核心已经占用了 `TIM2_IRQHandler` 和 `USART1_IRQHandler`**。
   自己再定义会 `multiple definition` 链接失败。
   定时器改用核心的 `HardwareTimer` 类注册回调；串口改用轮询 `Serial1.read()`
   （原因见 `link_uart.cpp` 顶部注释）。

4. **CAN 波特率照抄 F4 例程会不通**。达妙例程是 F4（APB1 = 42 MHz）的
   `BS1=15TQ/BS2=5TQ`；搬到 F103（APB1 = 36 MHz）算出 857 kbps。
   正确值 `TS1=15 / TS2=2` 才能得到整 1 Mbps。详见手册第五章。

5. **路径含中文会踩两次**（这两个是不同的问题，别混）：

   **(a) 构建目录含中文 → STM32 镜像生成失败（硬错误）**。
   PlatformIO 时代 5 组对照实验已确认与路径长度无关、与非 ASCII 字符有关
   （当时现象是 `arm-none-eabi-ld: cannot open map file`）。换成 CMake 后把根因
   精确定位了：**链接是成功的**，失败的是紧接着的 `arm-none-eabi-objcopy`
   （ELF → `.bin/.hex`）—— Windows 的 GNU binutils 按 ANSI 代码页解析路径，
   中文变成乱码文件名，报 `No such file or directory`。
   已写进预设：`stm32-f103` 的 `binaryDir` 指向纯 ASCII 的
   `C:/cmake_build/target-vehicle/stm32`。ESP32 不受影响（镜像由 `esptool.py` 生成）。

   **(b) 源码目录含中文 → 增量编译失效（只是慢）**。
   Ninja 在 Windows 上读不了 `.d` 依赖文件里带中文的路径，每次构建都判定
   `deps are missing`，于是**每次全量重编**（STM32 约 40 秒，ESP32 几分钟）。
   对照实验：同一工程放在 `C:\target-vehicle` 第二次构建输出 `ninja: no work to do.`；
   放在 `C:\Users\zzq\Desktop\靶车` 第二次仍然 `[13/13]` 全部重编。
   解法是把工程目录改成纯英文名（`D:\target-vehicle`），源码和 CMakeLists 都不用改。
   配置时 CMake 会专门为此给一条警告。

6. **M5Stamp-S3Bat DIP 的 G43/G44 被电源管理芯片占用**，
   不能当普通串口用来接 STM32，已改用排针上的 G1/G2。

7. **核心的 RX 回调是独占的**。`uart_attach_rx_callback()` 一旦注册，
   HAL 的 `HAL_UART_RxCpltCallback` 只调用用户回调、不再往 Arduino 环形缓冲塞字节。
   所以"注册回调"和"`Serial1.read()`"二者只能选一个，不能混用。

8. **迁移到 CMake 过程中新踩的坑**（共 11 条，含 `CMAKE_EXECUTABLE_SUFFIX` 被改成 `.elf`
   导致编译器路径变成 `gcc.elf`、`-T"路径"` 在 cmd.exe 下失效、core 的 glob 漏掉
   `startup_stm32yyxx.S` 导致 `cannot find entry symbol Reset_Handler`、
   `CMAKE_CURRENT_LIST_DIR` 在函数里指向调用者目录导致 monitor 找不到脚本、
   `CONFIGURE_DEPENDS` 让每次构建都重跑 CMake 等），
   全部整理在 [CMake 构建说明](docs/CMake构建说明.md) 第九节，并就地写在对应文件注释里。

---

## 资料对应关系

| 你提供的资料 | 用到了什么 |
|---|---|
| `DM-G6220电机资料.zip` | 官方例程 `dm_motor_drv.c/.h` → MIT 字节布局、量程、CAN ID 规则、1 Mbps |
| `M5Stack_Stamp-S3Bat_DIP_官方资料.zip` | 官方说明第 7 页「管脚映射」→ 确认 G43/G44 冲突，定下 G1/G2 接线 |
| `SN65HVD230_CAN模块资料.zip` | 收发器接线（CTX/CRX/CANH/CANL）与 3.3V 供电要求 |
| `野火STM32F103C8T6核心板双USB款.zip` | 板卡确认（F103C8T6 = bluepill 同芯片同引脚）、PA9/PA10/PA11/PA12 可用性 |
