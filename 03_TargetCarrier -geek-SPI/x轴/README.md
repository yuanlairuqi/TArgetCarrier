# X轴手动连续运动

2026-10-20：新增 Android 手动控制端，STM32 保持 FreeRTOS-Kernel V11.2.0，ESP 保持原协议与源码。接线不变。

## 使用

1. 将 `交付文件/STM32-XAxis-Continuous-Release.hex` 烧录到野火 STM32（Flash 基址0x08000000）。本次仅生成文件，未自动烧录。
2. 手机安装 `交付文件/TargetCarrier-XAxis-v3.3-debug.apk`，开启蓝牙并搜索 ESP32-Robot，允许附近设备权限。
3. 等待首次停止同步完成。设定速度，点击正转启动／反转启动；会持续发送200ms心跳并连续运动，点击停止结束。换向先关闭脉冲，等待保持时间、切换DIR、等待建立时间后恢复。
4. 切后台、退出页面、断连或通信超时会停止控制；重新连接需要重新同步 STOP，不恢复旧动作。

只有“正向／反向脉冲输出已确认”才表示 STM32 软件输出匹配本次手机请求；这不是电机实测运动、位置或到位反馈。速度为频率指令，1–100% 默认100–1000Hz，0%停止。实际正反方向、驱动器接口、允许频率需在硬件上确认；当前未完成实机连续运动验证。

旧 STM32 固件连续功能关闭，单独安装 APK 不能开启它。ESP 协议未变，已烧录的主工程 S3 固件可继续使用。

## 连线

| 用途 | 连接 |
| --- | --- |
| UART | S3 G4 TX → STM32 PB11 RX；S3 G5 RX ← STM32 PB10 TX；GND共地 |
| PUL采样 | STM32 PA8 → S3 G6 |
| DIR采样 | STM32 PB0 → S3 G7 |
| 电机输出 | PA8为PUL，PB0为DIR，沿用现有驱动器接口接线 |

未配置 ENA；停止或上电不发脉冲不等于驱动器禁能。上电定量动作现已默认关闭。

## 配置与构建

STM32：`野火stm/App/Motion/motor_config.h` 是配置入口。CMake 的 `TARGETCARRIER_CONTINUOUS_ENABLED=ON` 与 `TARGETCARRIER_STARTUP_ONESHOT=OFF` 分别产生 `MOTOR_CONTINUOUS_ENABLED=1` 与 `MOTOR_STARTUP_ONESHOT_ENABLED=0`，使用 CMake 时以它的编译宏为准。宏变更需重新构建和烧录，不能在线修改。

在野火stm目录构建：

```powershell
python tools/build_stm32.py --config Release --continuous ON --startup-oneshot OFF
```

本机中文路径由脚本自动复制到临时ASCII路径编译，产物回到 `野火stm/build/Release`。工具不在 PATH 时传 `--cmake`、`--gcc`、`--ninja` 完整路径。详见 `野火stm/FREERTOS_PORT.md`。

Android 仍使用 Android Java 工具链，构建命令见 `android/README.md`；STM32 与 ESP 固件各自使用 CMake，不共用交叉工具链。

## 回归验证

```powershell
python tools/test_manual_control.py --gcc 'D:/mingw/mingw64/bin/gcc.exe' --java-home 'D:/uu'
```

包含20组STM32寄存器／UART／RTOS启动模型测试，以及5组Android确认、队列、协议与波形解码测试。模型测试与编译通过不能证明实际电机运动。结果写入 `build/manual_control_tests`；本轮交付记录见 `verification/manual_control_20261020`。


## OLED status display (2026-10-21)

双OLED功能与宏配置见 [OLED说明](OLED说明.md)，构建和测试记录见 [RESULT.json](verification/oled_20261021/RESULT.json)。本轮未烧录，实物型号和显示仍待验证。
