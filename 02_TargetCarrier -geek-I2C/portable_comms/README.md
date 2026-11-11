# ESP32 / STM32 可移植通信包 TC/2

本次工作副本：`C:\Users\y\Desktop\TargetCarrier -统筹`。原工程 `C:\Users\y\Desktop\TargetCarrier` 未作为写入或构建输出目录。

本包是独立通信参考程序：GEEK USB 发射端 → 加密 ESP-NOW → ESP32-S3 接收端 → **所选一种** UART / SPI / I²C → STM32F103C8T6。上行方向相反。两个 ESP32 角色使用同一套 C 模块和按角色编译的入口。

旧 GU/1、WN/1 和电机业务保持在原有副本目录中，新 TC/2 固件不包含 OLED、电机、CAN 或旧业务。统一控制台保留旧功能，增加“通用调参 TC/2”入口，打开独立调参窗口。两套协议不能同时占用同一 COM 口。使用旧固件时进入原控制页；使用本包三端新固件时进入 TC/2 窗口。没有把新协议的 ACK 映射成旧电机的 APPLIED。

本次设计选择：统一上层协议，构建时 `TC_BACKEND=1/2/3` 互斥选择一种有线后端。未实现并行接口、自动切换、自动发现设备或参数断电保存。

## 目录

```text
TargetCarrier -统筹/
├─ ESP32-S3-GEEK/                     既有 GU/1、WN/1 总机（保留）
├─ x轴/                              既有从机、HAL / FreeRTOS STM32（保留）
├─ 靶车/                             既有 STM32duino / FreeRTOS 工程（保留）
├─ portable_comms/
│  ├─ core/tc.h                      C 公共 API、尺寸和类型
│  ├─ core/tc_protocol.c             编解码、流解析、重组、有界队列
│  ├─ core/tc_params.c               会话、参数注册表、去重与业务回调
│  ├─ core/tc_wave.c                 波形编码、采集缓冲、限流与背压
│  ├─ config/tc_board.h              一种后端、速率与参考引脚
│  ├─ config/tc_pairing.example.h     禁用的无线配置示例
│  ├─ platform/esp_radio.c           加密 ESP-NOW、HMAC、回调队列
│  ├─ platform/esp_bus.c             ESP-IDF UART/SPI/I²C 主端适配
│  ├─ platform/stm32_bus.c           F103 DMA / 中断从端适配
│  ├─ firmware/esp32/               两种角色，独立 ESP-IDF 工程
│  ├─ firmware/stm32/               独立 F103 CMake 工程与最小 main
│  ├─ reference/stm32/              实际副本的 CMSIS、启动、链接和时钟源
│  ├─ pc/                          无界面依赖的 Python 协议与独立窗口
│  ├─ tests/                       生产 C/平台源测试、C↔Python 故障注入
│  ├─ tools/build.py               独立复现构建（不含烧录）
│  └─ docs/                       协议、预算、接线、移植和验收
└─ 上位机/TargetCarrier-Unified/
   ├─ source/                      从既有统一版复制后修改
   ├─ app/TargetCarrier-Unified/    Vue 统一控制台 EXE + _internal
   ├─ TC-Portable/                 TC/2 窗口 EXE + _internal
   └─ verification/                原上位机源码复制校验清单
```

## 入口

1. [协议和兼容策略](docs/PROTOCOL.md)
2. [接口配置、接线与资源](docs/INTERFACES.md)
3. [原链路盘点、计算方法与建议值](docs/BANDWIDTH.md)
4. [模块职责、公共 API、最小接入与移植](docs/PORTING.md)
5. [验证范围与硬件验收](docs/VALIDATION.md)

当前无线默认禁用：没有捏造实际 MAC、信道或密钥。只有提供真实配置、重新编译后，才能进行无线链路验收。生成镜像、协议模拟及界面启动均不等于物理通信或采样成功。

Windows上位机请使用“启动统一控制台”或“启动通用调参”快捷入口。本机.NET加载器在中文路径下失败，已将与统筹副本哈希一致的EXE及`_internal`放入`C:\Users\y\Documents\Codex\work\TC2-runtime`作为ASCII运行目录；源码、完整EXE与验证记录仍保留在统筹副本。两个窗口及统一版的调参入口已在该运行目录通过检查。该处理未修改系统配置。
